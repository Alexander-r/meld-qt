#include "iohelpers.h"

#include <QDir>
#include <QStringList>

namespace meld {

namespace path {

QString normalize(const QString &path)
{
    if (path.isEmpty())
        return path;
    QString cleaned = QDir::cleanPath(QDir::fromNativeSeparators(path));
    if (cleaned.size() > 1 && cleaned.endsWith(QLatin1Char('/')) && !cleaned.endsWith(QLatin1String(":/")))
        cleaned.chop(1);
    return cleaned;
}

static bool isRoot(const QString &path)
{
    return path == QLatin1String("/") || (path.size() == 3 && path.at(1) == QLatin1Char(':')
                                           && path.at(2) == QLatin1Char('/'));
}

std::optional<QString> parent(const QString &rawPath)
{
    const QString p = normalize(rawPath);
    if (p.isEmpty() || isRoot(p))
        return std::nullopt;
    const qsizetype slash = p.lastIndexOf(QLatin1Char('/'));
    if (slash < 0)
        return std::nullopt;
    if (slash == 0)
        return QStringLiteral("/");
    if (slash == 2 && p.at(1) == QLatin1Char(':'))
        return p.left(3);
    return p.left(slash);
}

std::optional<QString> relativePath(const QString &rawParent, const QString &rawDescendant)
{
    const QString parentPath = normalize(rawParent);
    const QString descendant = normalize(rawDescendant);
    if (parentPath.isEmpty() || descendant.isEmpty() || parentPath == descendant)
        return std::nullopt;
    QString prefix = parentPath;
    if (!prefix.endsWith(QLatin1Char('/')))
        prefix += QLatin1Char('/');
    if (!descendant.startsWith(prefix))
        return std::nullopt;
    return descendant.mid(prefix.size());
}

QString basename(const QString &rawPath)
{
    const QString p = normalize(rawPath);
    if (isRoot(p))
        return p;
    const qsizetype slash = p.lastIndexOf(QLatin1Char('/'));
    return slash < 0 ? p : p.mid(slash + 1);
}

bool hasParent(const QString &p)
{
    return parent(p).has_value();
}

QString buildFilename(const QStringList &segments)
{
    // Equivalent of g_build_filenamev(): join with separators, collapsing
    // duplicate separators between elements.
    QString result;
    for (const QString &segment : segments) {
        if (segment.isEmpty())
            continue;
        if (result.isEmpty()) {
            result = segment;
            continue;
        }
        QString piece = segment;
        while (piece.startsWith(QLatin1Char('/')))
            piece.remove(0, 1);
        if (!result.endsWith(QLatin1Char('/')))
            result += QLatin1Char('/');
        result += piece;
    }
    return result;
}

} // namespace path

std::optional<QString> findSharedParentPath(const QList<std::optional<QString>> &paths)
{
    if (paths.isEmpty() || !paths.first())
        return std::nullopt;
    for (const auto &p : paths) {
        if (!p)
            return std::nullopt;
    }

    std::optional<QString> currentParent = path::parent(*paths.first());
    if (paths.size() == 1)
        return currentParent;

    while (currentParent) {
        bool isValidParent = true;
        for (const auto &p : paths) {
            if (!path::relativePath(*currentParent, *p)) {
                isValidParent = false;
                break;
            }
        }
        if (isValidParent)
            break;
        currentParent = path::parent(*currentParent);
    }

    // Either we've broken out of the loop early, in which case we have a
    // valid common parent path, or we've fallen through, in which case the
    // path return is None.
    return currentParent;
}

QString formatHomeRelativePath(const QString &p, const QString &homeDir)
{
    const QString home = homeDir.isEmpty() ? QDir::homePath() : homeDir;
    if (const auto relative = path::relativePath(home, p))
        return path::buildFilename({QStringLiteral("~"), *relative});
    return QDir::toNativeSeparators(path::normalize(p));
}

QString formatParentRelativePath(const QString &parentPath, const QString &descendant,
                                 const QString &homeDir)
{
    // When thinking about the segmentation we do here, there are four path
    // components that we care about:
    //
    //  * any path components above the non-common parent
    //  * the earliest non-common parent
    //  * any path components between the actual filename and the earliest
    //    non-common parent
    //  * the actual filename
    //
    // Of all of these, the first (the first common parent) is the *only*
    // one that's actually guaranteed to be the same. The second will
    // *always* be different (or won't exist if e.g., you're comparing files
    // in the same folder or similar).

    QString basePathStr;
    QStringList immediateParentStrs;
    bool hasElidedPath = false;

    const std::optional<QString> descendantParent = path::parent(descendant);
    if (!descendantParent)
        throw PathError(QStringLiteral("Path %1 has no parent").arg(descendant).toStdString());

    if (const auto relativePathStr = path::relativePath(parentPath, *descendantParent)) {
        const QStringList parts = relativePathStr->split(QLatin1Char('/'), Qt::SkipEmptyParts);

        // We always try to leave the first and last path segments, to try
        // to handle e.g., <parent>/<project>/src/<module>/main.py.
        basePathStr = parts.first();
        if (parts.size() > 1)
            immediateParentStrs.append(parts.last());

        // As an additional heuristic, we try to include the second-last path
        // segment as well, to handle paths like e.g.,
        // <parent>/<some package structure>/<module>/src/main.py. We only do
        // this if the last component is short, to handle src, dist, pkg,
        // etc. without using too much space.
        if (parts.size() > 2 && immediateParentStrs.first().size() < 5)
            immediateParentStrs.prepend(parts.at(parts.size() - 2));

        // We have elided paths if we have more parts than our immediate
        // parent count plus one for the base component.
        const qsizetype includedPathCount = immediateParentStrs.size() + 1;
        hasElidedPath = parts.size() > includedPathCount;
    }

    const bool showParent = !path::hasParent(parentPath);
    // It looks odd to have a single component, so if we don't have a base
    // path, we'll use the direct parent. In this case the parent won't
    // provide any disambiguation... it's just for appearances.
    if (!showParent && basePathStr.isEmpty())
        basePathStr = path::basename(parentPath);

    const QString ellipsis = QStringLiteral("…");
    QStringList labelSegments;
    if (!showParent)
        labelSegments.append(ellipsis);
    labelSegments.append(basePathStr);
    if (hasElidedPath)
        labelSegments.append(ellipsis);
    labelSegments.append(immediateParentStrs);
    labelSegments.append(path::basename(descendant));

    QString labelText = showParent ? formatHomeRelativePath(parentPath, homeDir) : QString();
    labelText += path::buildFilename(labelSegments);
    return labelText;
}

} // namespace meld
