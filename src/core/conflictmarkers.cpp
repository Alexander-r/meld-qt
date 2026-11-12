// Reading files with merge conflict markers

#include "conflictmarkers.h"

#include <QCoreApplication>

namespace meld::conflict {

namespace {

constexpr int MinimumMarkerSize = 7;

//! tr() for the code here that isn't in a QObject class
struct Tr {
    Q_DECLARE_TR_FUNCTIONS(meld::conflict)
};

QByteArray withoutEnding(const QByteArray &line)
{
    qsizetype size = line.size();
    if (size > 0 && line.at(size - 1) == '\n')
        --size;
    if (size > 0 && line.at(size - 1) == '\r')
        --size;
    return line.left(size);
}

/*!
 * Whether a line is a marker of `size` characters `c` (any size of at
 * least 7, if `size` is 0), alone or followed by a space and a label
 */
bool isMarker(const QByteArray &line, char c, int size, int *foundSize = nullptr, QByteArray *label = nullptr)
{
    const QByteArray content = withoutEnding(line);
    qsizetype count = 0;
    while (count < content.size() && content.at(count) == c)
        ++count;
    if (size > 0 ? count != size : count < MinimumMarkerSize)
        return false;
    if (count < content.size() && content.at(count) != ' ')
        return false;
    if (foundSize)
        *foundSize = int(count);
    if (label)
        *label = content.mid(count).trimmed();
    return true;
}

} // namespace

std::optional<Versions> parse(const QByteArray &data, bool includeAncestor, QString *error)
{
    enum class Part {
        Common,
        Local,
        Ancestor,
        Remote,
    };

    Versions versions;
    versions.hasAncestor = true;
    Part part = Part::Common;
    int size = 0;
    int start = 0;
    bool regionHasAncestor = false;
    QByteArray ancestor;
    const auto fail = [error](const QString &message) -> std::optional<Versions> {
        if (error)
            *error = message;
        return std::nullopt;
    };

    int lineNumber = 0;
    qsizetype position = 0;
    while (position < data.size()) {
        qsizetype end = data.indexOf('\n', position);
        end = end < 0 ? data.size() : end + 1;
        const QByteArray line = data.mid(position, end - position);
        position = end;
        ++lineNumber;

        QByteArray label;
        switch (part) {
        case Part::Common:
            if (isMarker(line, '<', 0, &size, &label)) {
                part = Part::Local;
                start = lineNumber;
                regionHasAncestor = false;
                ancestor.clear();
                if (versions.localLabel.isEmpty())
                    versions.localLabel = QString::fromUtf8(label);
                continue;
            }
            versions.local += line;
            versions.remote += line;
            versions.merged += line;
            continue;
        case Part::Local:
            if (isMarker(line, '|', size, nullptr, &label)) {
                part = Part::Ancestor;
                regionHasAncestor = true;
                if (versions.ancestorLabel.isEmpty())
                    versions.ancestorLabel = QString::fromUtf8(label);
                continue;
            }
            if (isMarker(line, '=', size, nullptr, &label) && label.isEmpty()) {
                part = Part::Remote;
                continue;
            }
            break;
        case Part::Ancestor:
            if (isMarker(line, '=', size, nullptr, &label) && label.isEmpty()) {
                part = Part::Remote;
                continue;
            }
            break;
        case Part::Remote:
            if (isMarker(line, '>', size, nullptr, &label)) {
                part = Part::Common;
                ++versions.conflicts;
                versions.hasAncestor = versions.hasAncestor && regionHasAncestor;
                if (versions.remoteLabel.isEmpty())
                    versions.remoteLabel = QString::fromUtf8(label);
                // The ancestor is only shown if every conflict has it
                if (includeAncestor)
                    versions.merged += ancestor;
                continue;
            }
            break;
        }

        // In a conflict, a new one can't start
        if (isMarker(line, '<', size)) {
            return fail(Tr::tr("Line %1: a conflict starts inside the conflict that starts at line %2.")
                            .arg(lineNumber)
                            .arg(start));
        }
        if (part == Part::Local)
            versions.local += line;
        else if (part == Part::Ancestor)
            ancestor += line;
        else
            versions.remote += line;
    }

    if (part != Part::Common)
        return fail(Tr::tr("The conflict that starts at line %1 doesn’t end.").arg(start));
    if (versions.conflicts == 0) {
        if (error)
            error->clear();
        return std::nullopt;
    }
    if (!versions.hasAncestor && includeAncestor) {
        // Without the ancestor in every conflict, none is shown
        return parse(data, false, error);
    }
    versions.ancestorIncluded = includeAncestor && versions.hasAncestor;
    return versions;
}

int countConflicts(const QByteArray &data)
{
    // A quick check first, as most files have no conflicts
    if (!data.contains("<<<<<<<"))
        return 0;
    const std::optional<Versions> versions = parse(data, false);
    return versions ? versions->conflicts : 0;
}

} // namespace meld::conflict
