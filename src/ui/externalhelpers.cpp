#include "externalhelpers.h"

#include "settings.h"

#include <QDesktopServices>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QMessageBox>
#include <QProcess>
#include <QRegularExpression>
#include <QUrl>

namespace meld {

Q_LOGGING_CATEGORY(lcExternal, "meld.external")

QStringList makeCustomEditorCommand(const QString &path, int line)
{
    const QString command = MeldSettings::instance()->stringValue(QStringLiteral("custom-editor-command"));
    static const QRegularExpression field(QStringLiteral("\\{([^{}]*)\\}"));
    bool hasFields = false;
    bool supported = true;
    for (auto it = field.globalMatch(command); it.hasNext();) {
        const QString name = it.next().captured(1);
        hasFields = true;
        if (name != QLatin1String("file") && name != QLatin1String("line"))
            supported = false;
    }

    if (!hasFields)
        return {command, path};
    if (!supported) {
        qCWarning(lcExternal) << "Unsupported fields found";
        return {command, path};
    }

    // Split like shlex: substitute after splitting so that paths with
    // spaces stay a single argument.
    QStringList result;
    for (QString part : QProcess::splitCommand(command)) {
        part.replace(QStringLiteral("{file}"), path);
        part.replace(QStringLiteral("{line}"), QString::number(line));
        result.append(part);
    }
    return result;
}

void openFilesExternal(const QStringList &paths, QWidget *parent, int line)
{
    MeldSettings *settings = MeldSettings::instance();
    for (const QString &path : paths) {
        const QFileInfo info(path);
        if (info.isDir()) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(path));
            continue;
        }
        if (!info.isFile()) {
            QMessageBox::warning(parent, QObject::tr("Unsupported file type"),
                                 QObject::tr("External opening of files of type “%1” is not supported")
                                     .arg(QObject::tr("unknown")));
            continue;
        }

        if (settings->boolValue(QStringLiteral("use-system-editor"))
            || settings->stringValue(QStringLiteral("custom-editor-command")).isEmpty()) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(path));
            continue;
        }

        const QStringList command = makeCustomEditorCommand(path, line);
        if (command.isEmpty() || !QProcess::startDetached(command.first(), command.mid(1))) {
            QMessageBox::warning(parent, QObject::tr("Failed to launch custom editor"),
                                 QObject::tr("Couldn't run “%1”").arg(command.join(QLatin1Char(' '))));
        }
    }
}

} // namespace meld
