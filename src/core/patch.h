// Reading patches and applying them in memory
//
// A file or folder can be compared with what it would be with
// a patch applied: the patch is read here, and applied to the files
// in memory as they're compared, without the patch program or
// temporary files.
//
// Patches are unified diffs, as made by diff -u, git diff and git
// format-patch, and most version control systems; git's extended headers
// (new, deleted, renamed and copied files) are understood. Hunks are applied
// as GNU patch does by default: where the patch says if the file matches
// there, or else at the nearest place it does (an offset), or else ignoring
// up to two lines of context at either end (fuzz). Line endings that differ
// between the patch and the file (CRLF and LF) are tolerated.

#pragma once

#include <QByteArray>
#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>

#include <memory>
#include <optional>
#include <vector>

namespace meld::patch {

struct HunkLine {
    //! ' ' for context, '-' for a removed line, '+' for an added one
    char kind = ' ';
    //! The line, with its line ending unless it has none
    QByteArray text;
};

struct Hunk {
    int oldStart = 0;
    int oldCount = 0;
    int newStart = 0;
    int newCount = 0;
    std::vector<HunkLine> lines;
};

//! The changes to one file
struct FilePatch {
    //! The paths in the patch, with their prefixes (e.g., "a/"); empty for
    //! /dev/null, as for new and deleted files. Backslashes are separators
    //! in patches whose paths have no '/' (as from Windows diff tools).
    QString oldPath;
    QString newPath;
    bool newFile = false;
    bool deletedFile = false;
    bool rename = false;
    bool copy = false;
    //! Binary changes, which can't be applied
    bool binary = false;
    std::vector<Hunk> hunks;
    //! The line of the patch where this file's changes start, from 1
    int line = 0;

    //! The path of the file the changes apply to, as given in the patch
    QString sourcePath() const { return oldPath.isEmpty() ? newPath : oldPath; }
    //! The path of the changed file, as given in the patch
    QString targetPath() const { return newPath.isEmpty() ? oldPath : newPath; }
};

struct Patch {
    std::vector<FilePatch> files;
};

/*!
 * Read a patch. Text that isn't part of a diff (e.g., a commit message) is
 * skipped. Returns nullopt, with an error, if the patch is malformed or has
 * no changes.
 */
std::optional<Patch> parse(const QByteArray &data, QString *error);
//! Read a patch file
std::optional<Patch> parseFile(const QString &path, QString *error);

struct ApplyResult {
    //! The patched text, or nullopt if the patch couldn't be applied
    std::optional<QByteArray> text;
    //! Whether the patch deletes the file
    bool deleted = false;
    //! Why the patch couldn't be applied
    QString error;
    //! How hunks were applied, if not exactly where the patch says
    QStringList notes;
};

//! Apply a file's changes to its text (that of an empty or missing file for
//! new files)
ApplyResult apply(const QByteArray &original, const FilePatch &patch);

//! A path without its first `level` components; nullopt if it has no more
std::optional<QString> stripPath(const QString &path, int level);

/*!
 * The changes to a file, among a patch's: those of the only file, or else of
 * the file whose path best matches the end of `file`'s. Returns nullptr,
 * with an error, if no file's changes match.
 */
const FilePatch *findFilePatch(const Patch &patch, const QString &file, QString *error);

/*!
 * A folder as it would be with a patch applied
 *
 * Which files the patch adds, changes and deletes is known from the patch;
 * a file's patched text is only made when it's asked for. Paths are relative
 * to the folder, with '/' separators.
 */
class PatchedFolder {
public:
    enum class Change {
        None,
        Modified,
        Added,
        Deleted,
    };

    /*!
     * Read a patch for a folder. The number of leading path components to
     * strip (as patch -p) is the one that finds the most files to change.
     * Returns nullptr, with an error, if the patch can't be read.
     */
    static std::shared_ptr<PatchedFolder> load(const QString &folder, const QString &patchFile, QString *error);

    QString folder() const { return folder_; }
    QString patchFile() const { return patchFile_; }
    int stripLevel() const { return strip_; }

    //! How the patch changes a path
    Change change(const QString &path) const;
    bool exists(const QString &path) const;
    bool isDir(const QString &path) const;
    bool isFile(const QString &path) const;
    //! The names in a patched folder
    QStringList entries(const QString &folder) const;
    //! Changes in a folder that can't be applied (e.g., to files that
    //! aren't there): names and why
    QList<std::pair<QString, QString>> errors(const QString &folder) const;
    //! The patched text of a file; files the patch doesn't change are read
    ApplyResult content(const QString &path) const;
    //! The file a patched file is made from; empty for new files
    QString source(const QString &path) const;

private:
    struct Target {
        Change change = Change::Modified;
        //! The file that the changes apply to; empty for new files
        QString source;
        std::vector<const FilePatch *> patches;
    };

    QString realPath(const QString &path) const;

    QString folder_;
    QString patchFile_;
    Patch patch_;
    int strip_ = 0;
    QHash<QString, Target> targets_;
    //! Folders that only exist for added files
    QSet<QString> addedFolders_;
    //! Folder → (name, error)
    QHash<QString, QList<std::pair<QString, QString>>> errors_;
};

} // namespace meld::patch
