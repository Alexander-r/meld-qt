#pragma once

#include <QStringList>

class QWidget;

namespace meld {

//! Build the custom editor command, substituting {file} and {line}
QStringList makeCustomEditorCommand(const QString &path, int line = 0);

//! Open files with the system's default application or the custom editor
void openFilesExternal(const QStringList &paths, QWidget *parent = nullptr, int line = 0);

} // namespace meld
