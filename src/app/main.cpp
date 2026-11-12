#include "core/translations.h"
#include "icons.h"
#include "meldapp.h"
#include "settings.h"
#ifdef MELD_SINGLE_INSTANCE
#include "singleinstance.h"
#endif
#include "style.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QIcon>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    // Before anything translatable, including the settings' defaults (the
    // names of filters) and the command line's help
    meld::translations::installSetting(meld::MeldSettings::languageSetting());
    QApplication::setApplicationName(QStringLiteral("Meld Qt"));
    QApplication::setApplicationDisplayName(QStringLiteral("Meld Qt"));
    QApplication::setApplicationVersion(QStringLiteral(MELD_VERSION));
    QGuiApplication::setDesktopFileName(QStringLiteral("Meld-Qt"));
    QApplication::setWindowIcon(QIcon::fromTheme(QStringLiteral("Meld-Qt"),
                                                 QIcon(QStringLiteral(":/icons/meld-qt.svg"))));

    // In single-instance mode, the running instance opens the comparisons,
    // and this invocation ends when they're closed. Help, version and usage
    // errors are handled here.
    const QStringList arguments = QApplication::arguments();
#ifdef MELD_SINGLE_INSTANCE
    const meld::CommandLineParseResult parsed =
        meld::parseCommandLine(arguments.mid(1), QFileInfo(arguments.value(0)).fileName());
    const bool singleInstance = meld::MeldSettings::instance()->boolValue(QStringLiteral("single-instance"));
    if (!parsed.shouldExit && meld::shouldForwardCommandLine(parsed.options, singleInstance)) {
        if (const std::optional<int> status = meld::forwardCommandLine(arguments, QDir::currentPath()))
            return *status;
    }
#endif

    meld::icons::initialiseIconTheme();
    meld::applyStyleVariant();

    meld::MeldApp meldApp;
    const int status = meldApp.handleCommandLine(arguments);
    if (status >= 0)
        return status;
#ifdef MELD_SINGLE_INSTANCE
    // Later invocations in single-instance mode (or, with local sockets,
    // with --newtab) open their comparisons here, unless another instance
    // already does this
    if (meld::shouldReceiveCommandLines(parsed.options, singleInstance))
        meldApp.startSingleInstanceServer(meld::SingleInstanceServer::instanceName());
#endif
    return QApplication::exec();
}
