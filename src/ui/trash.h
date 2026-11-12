#pragma once

#include <QString>

class QWidget;

namespace meld {

enum class TrashResult {
    Removed,
    Cancelled,
    Failed,
};

/*!
 * Trash or delete the given file or folder
 *
 * Files and folders are moved to the system Trash location without
 * confirmation. If they can't be trashed, then the user is prompted for an
 * irreversible deletion. On failure, `error` is set to the reason.
 */
TrashResult trashOrConfirm(const QString &path, QWidget *parent, QString *error = nullptr);

} // namespace meld
