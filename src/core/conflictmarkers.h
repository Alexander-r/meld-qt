// Reading files with merge conflict markers
//
// A file with unresolved merge conflicts, as version control systems
// leave them, has each conflict between markers:
//
//     <<<<<<< local label
//     the local version
//     ||||||| ancestor label        (diff3 style only)
//     the common ancestor's version
//     =======
//     the remote version
//     >>>>>>> remote label
//
// Outside the conflicts, the text is common to all versions, so the local
// and remote versions (and the common ancestor's, with diff3-style markers)
// can be made from the file alone. They're made in memory, as bytes, so
// encodings and line endings are kept.

#pragma once

#include <QByteArray>
#include <QString>

#include <optional>

namespace meld::conflict {

//! A file's versions, as made from its conflict markers
struct Versions {
    QByteArray local;
    QByteArray remote;
    /*!
     * The file to resolve the conflicts in: the common text, with the
     * common ancestor's text in conflicts if it's included, or nothing
     */
    QByteArray merged;
    //! Whether every conflict has the common ancestor's text (diff3 style)
    bool hasAncestor = false;
    //! Whether `merged` has it
    bool ancestorIncluded = false;
    int conflicts = 0;
    //! The labels after the markers, e.g., "HEAD" and a branch name
    QString localLabel;
    QString ancestorLabel;
    QString remoteLabel;
};

/*!
 * Read a file's conflict markers. Markers start their lines, and their
 * length (7 by default) is that of the first; they must come in order.
 * Returns nullopt if there are no conflicts, or, with an error, if the
 * markers are incomplete.
 *
 * With `includeAncestor`, the merged text has the common ancestor's text in
 * conflicts, where the markers have it.
 */
std::optional<Versions> parse(const QByteArray &data, bool includeAncestor, QString *error = nullptr);

//! The number of conflicts in a file with complete conflict markers, or 0
int countConflicts(const QByteArray &data);

} // namespace meld::conflict
