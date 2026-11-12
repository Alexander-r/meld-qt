#include "conflictcomparison.h"

#include "core/conflictmarkers.h"
#include "settings.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace meld {

namespace {

//! tr() for the code here that isn't in a QObject class
struct Tr {
    Q_DECLARE_TR_FUNCTIONS(meld::ConflictComparison)
};

enum class Side {
    Local,
    Merged,
    Remote,
};

//! One version of the file, made from its markers as the pane is loaded
VirtualFile versionOf(const QString &path, Side side, bool includeAncestor, const QString &label)
{
    VirtualFile file;
    file.label = label;
    file.load = [path, side, includeAncestor]() {
        VirtualFile::Content content;
        QFile handle(path);
        if (!handle.open(QIODevice::ReadOnly)) {
            content.errorTitle = Tr::tr("There was a problem opening the file “%1”.").arg(QDir::toNativeSeparators(path));
            content.error = handle.errorString();
            return content;
        }
        const QByteArray data = handle.readAll();
        QString error;
        const std::optional<conflict::Versions> versions = conflict::parse(data, includeAncestor, &error);
        if (!versions) {
            if (!error.isEmpty()) {
                content.errorTitle =
                    Tr::tr("The conflict markers in “%1” can’t be read.").arg(QDir::toNativeSeparators(path));
                content.error = error;
                return content;
            }
            // Resolved already: every version is the file
            content.data = data;
            return content;
        }
        switch (side) {
        case Side::Local:
            content.data = versions->local;
            break;
        case Side::Remote:
            content.data = versions->remote;
            break;
        case Side::Merged:
            content.data = versions->merged;
            content.noteTitle = Tr::tr("%n unresolved conflicts", nullptr, versions->conflicts);
            content.notes = {versions->ancestorIncluded
                                 ? Tr::tr("Conflicts show the common ancestor’s text, which the local and remote "
                                      "versions changed.")
                                 : Tr::tr("Conflicts are empty, to be filled from the local and remote versions.")};
            break;
        }
        return content;
    };
    return file;
}

QString sideLabel(const QString &basename, const QString &side, const QString &marker)
{
    return marker.isEmpty() ? QStringLiteral("%1 — %2").arg(basename, side)
                            : QStringLiteral("%1 — %2 (%3)").arg(basename, side, marker);
}

} // namespace

int conflictsInFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return 0;
    return conflict::countConflicts(file.readAll());
}

DiffOptions conflictResolution(const QString &path, std::function<void(const QString &)> resolve)
{
    MeldSettings *settings = MeldSettings::instance();
    const bool includeAncestor = settings->boolValue(QStringLiteral("conflict-show-ancestor"));
    const QString basename = QFileInfo(path).fileName();

    // The markers' labels, e.g., "HEAD" and a branch name
    QString localMarker;
    QString remoteMarker;
    {
        QFile file(path);
        if (file.open(QIODevice::ReadOnly)) {
            if (const std::optional<conflict::Versions> versions = conflict::parse(file.readAll(), false)) {
                localMarker = versions->localLabel;
                remoteMarker = versions->remoteLabel;
            }
        }
    }
    const QString localLabel = sideLabel(basename, Tr::tr("local"), localMarker);
    const QString remoteLabel = sideLabel(basename, Tr::tr("remote"), remoteMarker);

    ComparisonMeta meta;
    const bool localFirst =
        settings->stringValue(QStringLiteral("vc-merge-file-order")) == QLatin1String("local-merge-remote");
    const Side first = localFirst ? Side::Local : Side::Remote;
    const Side last = localFirst ? Side::Remote : Side::Local;
    meta.virtualFiles.insert(0, versionOf(path, first, includeAncestor, localFirst ? localLabel : remoteLabel));
    meta.virtualFiles.insert(1, versionOf(path, Side::Merged, includeAncestor, QString()));
    meta.virtualFiles.insert(2, versionOf(path, last, includeAncestor, localFirst ? remoteLabel : localLabel));
    meta.labels = localFirst ? QStringList{localLabel, QString(), remoteLabel}
                             : QStringList{remoteLabel, QString(), localLabel};
    meta.tabLabel = localFirst ? Tr::tr("%1 (local, merge, remote)").arg(basename)
                               : Tr::tr("%1 (remote, merge, local)").arg(basename);
    meta.warnUnresolved = true;
    if (resolve) {
        meta.promptResolve = true;
        meta.resolve = std::move(resolve);
    }

    DiffOptions options;
    options.mergeOutput = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    options.meta = meta;
    return options;
}

} // namespace meld
