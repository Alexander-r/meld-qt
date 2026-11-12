#include "melddoc.h"

#include <QPointer>

#include <QApplication>
#include <QLoggingCategory>

namespace meld {

Q_LOGGING_CATEGORY(lcDoc, "meld.doc")

QString recentTypeName(RecentType type)
{
    switch (type) {
    case RecentType::File:
        return QStringLiteral("File");
    case RecentType::Folder:
        return QStringLiteral("Folder");
    case RecentType::VersionControl:
        return QStringLiteral("Version control");
    case RecentType::Merge:
        return QStringLiteral("Merge");
    }
    return {};
}

std::optional<RecentType> recentTypeFromName(const QString &name)
{
    for (RecentType type : {RecentType::File, RecentType::Folder, RecentType::VersionControl, RecentType::Merge}) {
        if (recentTypeName(type) == name)
            return type;
    }
    return std::nullopt;
}

MeldDoc::MeldDoc(QWidget *parent)
    : QWidget(parent)
{
}

MeldDoc::~MeldDoc() = default;

QAction *MeldDoc::addAction(const QString &name, std::function<void()> callback)
{
    auto *action = new QAction(name, this);
    action->setObjectName(name);
    connect(action, &QAction::triggered, this, [callback = std::move(callback)]() {
        if (callback)
            callback();
    });
    actions_.insert(name, action);
    return action;
}

QAction *MeldDoc::addToggleAction(const QString &name, bool initial, std::function<void(bool)> callback)
{
    auto *action = new QAction(name, this);
    action->setObjectName(name);
    action->setCheckable(true);
    action->setChecked(initial);
    connect(action, &QAction::toggled, this, [callback = std::move(callback)](bool checked) {
        if (callback)
            callback(checked);
    });
    actions_.insert(name, action);
    return action;
}

void MeldDoc::removeAction(const QString &name)
{
    if (QAction *action = actions_.take(name))
        action->deleteLater();
}

QAction *MeldDoc::action(const QString &name) const
{
    return actions_.value(name);
}

void MeldDoc::setActionEnabled(const QString &name, bool enabled)
{
    QAction *a = action(name);
    if (!a) {
        qCWarning(lcDoc) << "No action" << name << "found";
        return;
    }
    a->setEnabled(enabled);
}

bool MeldDoc::actionState(const QString &name) const
{
    const QAction *a = action(name);
    if (!a) {
        qCWarning(lcDoc) << "No action" << name << "found";
        return false;
    }
    return a->isChecked();
}

void MeldDoc::setActionState(const QString &name, bool state)
{
    QAction *a = action(name);
    if (!a) {
        qCWarning(lcDoc) << "No action" << name << "found";
        return;
    }
    a->setChecked(state);
}

void MeldDoc::setState(State state)
{
    if (state == state_)
        return;
    const State old = state_;
    state_ = state;
    Q_EMIT stateChanged(old, state);
}

void MeldDoc::setTabLabel(const QString &title, const QString &tooltip)
{
    if (title == tabTitle_ && tooltip == tabTooltip_)
        return;
    tabTitle_ = title;
    tabTooltip_ = tooltip;
    Q_EMIT tabLabelChanged();
}

void MeldDoc::onContainerSwitchIn()
{
    if (QWidget *pane = focusPane()) {
        QPointer<QWidget> guard(pane);
        scheduler_.addOneShot([guard]() {
            if (guard)
                guard->setFocus();
        });
    }
}

void MeldDoc::actionStop()
{
    if (scheduler_.tasksPending())
        scheduler_.removeTask(scheduler_.currentTask());
}

void MeldDoc::requestClose()
{
    setState(State::Closing);
    scheduler_.removeAllTasks();
    Q_EMIT closed(0);
}

void MeldDoc::errorBell()
{
    QApplication::beep();
}

} // namespace meld
