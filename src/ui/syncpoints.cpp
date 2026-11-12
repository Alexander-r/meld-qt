#include "syncpoints.h"

#include <QLoggingCategory>
#include <QTextBlock>
#include <QTextDocument>

#include <algorithm>

namespace meld {

Q_LOGGING_CATEGORY(lcSyncpoints, "meld.syncpoints")

QString syncpointActionName(SyncpointAction action)
{
    switch (action) {
    case SyncpointAction::Move:
        return QStringLiteral("move-sync-point");
    case SyncpointAction::Delete:
        return QStringLiteral("remove-sync-point");
    case SyncpointAction::Match:
        return QStringLiteral("match-sync-point");
    case SyncpointAction::Add:
        return QStringLiteral("add-sync-point");
    case SyncpointAction::Disabled:
        return QStringLiteral("disabled-sync-point");
    }
    return {};
}

void Syncpoints::setDocuments(const QList<QTextDocument *> &documents)
{
    documents_ = documents;
    marks_.assign(size_t(documents.size()), {});
    cachedPoints_.clear();
    updateSyncpointState();
}

int Syncpoints::lineStart(int pane, int cursorPosition) const
{
    const QTextBlock block = documents_.at(pane)->findBlock(cursorPosition);
    return block.isValid() ? block.position() : 0;
}

int Syncpoints::lineOf(int pane, const Mark &mark) const
{
    return documents_.at(pane)->findBlock(mark.cursor.position()).blockNumber();
}

const Syncpoints::Mark *Syncpoints::findMark(int pane, int id) const
{
    for (const Mark &mark : marks_[size_t(pane)]) {
        if (mark.id == id)
            return &mark;
    }
    return nullptr;
}

void Syncpoints::sortMarks(int pane)
{
    auto &marks = marks_[size_t(pane)];
    std::stable_sort(marks.begin(), marks.end(),
                     [](const Mark &a, const Mark &b) { return a.cursor.position() < b.cursor.position(); });
}

void Syncpoints::updateSyncpointState()
{
    // Walk syncpoint marks in all panes in lockstep, stopping whenever any
    // pane runs out of syncpoint marks
    for (size_t pane = 0; pane < marks_.size(); ++pane)
        sortMarks(int(pane));

    size_t paired = marks_.empty() ? 0 : marks_.front().size();
    for (const auto &marks : marks_)
        paired = std::min(paired, marks.size());

    bool allExhausted = true;
    for (const auto &marks : marks_)
        allExhausted = allExhausted && marks.size() == paired;

    if (allExhausted) {
        // Every pane ran out on the same step, so we have a matched state
        // and will use the list of syncpoints we've built
        paneStates_.assign(marks_.size(), PaneState::Matched);
        cachedPoints_.clear();
        for (size_t i = 0; i < paired; ++i) {
            std::vector<int> point;
            for (const auto &marks : marks_)
                point.push_back(marks[i].id);
            cachedPoints_.push_back(point);
        }
    } else {
        // Exhausted panes are SHORT, the rest are DANGLING
        paneStates_.clear();
        for (const auto &marks : marks_)
            paneStates_.push_back(marks.size() == paired ? PaneState::Short : PaneState::Dangling);
        // Drop any cached pairing that refers to a removed mark
        std::erase_if(cachedPoints_, [this](const std::vector<int> &point) {
            for (size_t pane = 0; pane < point.size() && pane < marks_.size(); ++pane) {
                if (!findMark(int(pane), point[pane]))
                    return true;
            }
            return false;
        });
    }
}

int Syncpoints::markAtCursor(int pane, int cursorPosition) const
{
    const int start = lineStart(pane, cursorPosition);
    for (const Mark &mark : marks_[size_t(pane)]) {
        if (mark.cursor.position() == start)
            return mark.id;
    }
    return -1;
}

void Syncpoints::add(int pane, int cursorPosition)
{
    QTextCursor cursor(documents_.at(pane));
    cursor.setPosition(lineStart(pane, cursorPosition));
    // Like GtkSourceMarks, syncpoints have left gravity
    cursor.setKeepPositionOnInsert(true);
    marks_[size_t(pane)].push_back({nextId_++, cursor});
    updateSyncpointState();
}

void Syncpoints::move(int pane, int cursorPosition)
{
    std::vector<int> matched;
    for (const auto &point : cachedPoints_)
        matched.push_back(point[size_t(pane)]);
    for (Mark &mark : marks_[size_t(pane)]) {
        if (std::find(matched.begin(), matched.end(), mark.id) == matched.end()) {
            mark.cursor.setPosition(lineStart(pane, cursorPosition));
            updateSyncpointState();
            return;
        }
    }
    qCWarning(lcSyncpoints) << "No unmatched syncpoint found to move";
}

void Syncpoints::remove(int pane, int cursorPosition)
{
    const int id = markAtCursor(pane, cursorPosition);
    if (id < 0) {
        qCWarning(lcSyncpoints) << "No syncpoint mark found when removing syncpoint";
        return;
    }
    std::erase_if(marks_[size_t(pane)], [id](const Mark &mark) { return mark.id == id; });
    updateSyncpointState();
}

void Syncpoints::clear()
{
    for (auto &marks : marks_)
        marks.clear();
    cachedPoints_.clear();
    updateSyncpointState();
}

std::vector<std::vector<int>> Syncpoints::validPoints() const
{
    std::vector<std::vector<int>> result;
    for (const auto &point : cachedPoints_) {
        std::vector<int> lines;
        for (size_t pane = 0; pane < point.size(); ++pane) {
            const Mark *mark = findMark(int(pane), point[pane]);
            lines.push_back(mark ? lineOf(int(pane), *mark) : 0);
        }
        result.push_back(lines);
    }
    return result;
}

SyncpointAction Syncpoints::action(int pane, int cursorPosition) const
{
    if (pane < 0 || size_t(pane) >= paneStates_.size())
        return SyncpointAction::Disabled;
    const PaneState state = paneStates_[size_t(pane)];
    const int mark = markAtCursor(pane, cursorPosition);

    switch (state) {
    case PaneState::Short:
        return mark >= 0 ? SyncpointAction::Disabled : SyncpointAction::Match;
    case PaneState::Matched:
        return mark >= 0 ? SyncpointAction::Delete : SyncpointAction::Add;
    case PaneState::Dangling:
        if (mark < 0)
            return SyncpointAction::Move;
        for (const auto &point : cachedPoints_) {
            if (point[size_t(pane)] == mark)
                return SyncpointAction::Disabled;
        }
        return SyncpointAction::Delete;
    }
    return SyncpointAction::Disabled;
}

std::vector<int> Syncpoints::lines(int pane) const
{
    std::vector<int> result;
    if (pane < 0 || size_t(pane) >= marks_.size())
        return result;
    for (const Mark &mark : marks_[size_t(pane)])
        result.push_back(lineOf(pane, mark));
    return result;
}

} // namespace meld
