#pragma once

#include <QKeySequence>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

namespace meld {

/*!
 * Keyboard shortcuts for actions, in QKeySequence::PortableText format.
 * Action names follow the GTK version's "group.name" convention.
 */
const QList<QPair<QString, QStringList>> &viewAccelerators();

QList<QKeySequence> shortcutsForAction(const QString &action);

} // namespace meld
