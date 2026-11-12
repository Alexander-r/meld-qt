#pragma once

#include "melddoc.h"

#include <functional>

namespace meld {

//! The number of conflicts in a file's conflict markers, or 0 if it has none
//! or can't be read
int conflictsInFile(const QString &path);

/*!
 * A 3-way merge of the conflicts in a file with conflict markers
 *
 * The local and remote versions are made from the markers in memory, and
 * are read-only. The middle pane is the merge, which is saved to the file:
 * the text outside conflicts, with the common ancestor's text in conflicts
 * if the markers have it and the "conflict-show-ancestor" setting is on, or
 * otherwise nothing. The comparison's files are all empty: they're made as
 * the comparison loads, from the file as it is then. `resolve`, if given,
 * marks the file as resolved in version control once it's saved.
 */
DiffOptions conflictResolution(const QString &path, std::function<void(const QString &)> resolve = {});

} // namespace meld
