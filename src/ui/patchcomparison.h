// Comparing files and folders with what a patch would make of them

#pragma once

#include "melddoc.h"

#include <memory>

namespace meld {

namespace patch {
class PatchedFolder;
}

//! The label of a file's pane, with a patch applied
QString patchedLabel(const QString &name, const QString &patchFile);
//! The tab label of a file's comparison with its patched text
QString patchComparisonTabLabel(const QString &file, const QString &patchFile);

/*!
 * A pane with a file's text as a patch changes it. The patch is read, and
 * applied to the file, whenever the pane is loaded.
 */
VirtualFile patchedFile(const QString &file, const QString &patchFile);
//! A pane with a file's text in a patched folder
VirtualFile patchedFile(const std::shared_ptr<const patch::PatchedFolder> &folder, const QString &path);

} // namespace meld
