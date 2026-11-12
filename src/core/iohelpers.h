#pragma once

#include <QList>
#include <QString>

#include <optional>
#include <stdexcept>

namespace meld {

//! Paths are absolute local file system paths. The helpers mirror the
//! GFile semantics used by the original implementation.
namespace path {

QString normalize(const QString &path);
std::optional<QString> parent(const QString &path);
//! GFile.get_relative_path(): only for strict descendants
std::optional<QString> relativePath(const QString &parent, const QString &descendant);
QString basename(const QString &path);
bool hasParent(const QString &path);
QString buildFilename(const QStringList &segments);

} // namespace path

std::optional<QString> findSharedParentPath(const QList<std::optional<QString>> &paths);

QString formatHomeRelativePath(const QString &path, const QString &homeDir = QString());

class PathError : public std::invalid_argument {
public:
    using std::invalid_argument::invalid_argument;
};

/*!
 * Format shortened child paths using a common parent
 *
 * This is a helper for shortening sets of paths using their common parent
 * as a guide for what is required to distinguish the paths from one
 * another. Throws PathError if `descendant` has no parent.
 */
QString formatParentRelativePath(const QString &parent, const QString &descendant,
                                 const QString &homeDir = QString());

} // namespace meld
