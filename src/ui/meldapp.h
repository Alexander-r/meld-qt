#pragma once

#include <QDir>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QStringList>

#include <functional>
#include <optional>

namespace meld {

class MeldDoc;
class MeldWindow;
class PreferencesDialog;
class RemoteCommandLine;
class SingleInstanceServer;

struct CommandLineOptions {
    QStringList labels;
    bool newTab = false;
    //! --single-instance or --no-single-instance, which override the
    //! "single-instance" setting for this invocation
    std::optional<bool> singleInstance;
    bool autoCompare = false;
    bool unified = false;
    QString outFile;
    bool autoMerge = false;
    QString comparisonFile;
    //! A patch to apply to the file or folder in `args`, to compare with
    QString patch;
    //! Extra comparisons given with --diff
    QList<QStringList> diffs;
    //! Positional arguments
    QStringList args;
};

struct CommandLineParseResult {
    //! Whether the program should exit without opening a window
    bool shouldExit = false;
    int exitStatus = 0;
    //! Text to print (help, version or an error with usage)
    QString output;
    CommandLineOptions options;
};

//! Parse arguments like meld's optparse-based parser; `arguments` excludes
//! the program name. Positional argument validity (the number of arguments
//! and auto-merge restrictions) is checked here too.
CommandLineParseResult parseCommandLine(const QStringList &arguments, const QString &programName = QStringLiteral("meld-qt"),
                                        const QString &cwd = QDir::currentPath());

/*!
 * Whether an invocation should open its comparisons in a running instance:
 * in single-instance mode (the setting, unless the command line overrides
 * it), and with local sockets also with --newtab, which asks for a new tab
 * in the running instance. Always false without single-instance support.
 */
bool shouldForwardCommandLine(const CommandLineOptions &options, bool singleInstanceSetting);
/*!
 * Whether an instance should receive the command lines of later
 * invocations: with shared memory, only in single-instance mode; with local
 * sockets, always.
 */
bool shouldReceiveCommandLines(const CommandLineOptions &options, bool singleInstanceSetting);

QString commandLineUsage(const QString &programName);
QString commandLineHelp(const QString &programName);

class MeldApp : public QObject {
    Q_OBJECT

public:
    explicit MeldApp(QObject *parent = nullptr);
    ~MeldApp() override;

    //! The most recently focused window
    MeldWindow *window() const;
    QList<MeldWindow *> windows() const;

    //! Handle the command line; returns -1 to keep running, or the exit
    //! status if the application should exit immediately.
    int handleCommandLine(const QStringList &arguments);

#ifdef MELD_SINGLE_INSTANCE
    /*!
     * Receive the command lines of later invocations, which run in this
     * instance (do_command_line() for remote command lines). Returns false
     * if another instance does this.
     */
    bool startSingleInstanceServer(const QString &name);
    SingleInstanceServer *singleInstanceServer() const { return server_; }
    void handleRemoteCommandLine(RemoteCommandLine *commandLine);
#endif

    void showPreferences();
    void showAbout();
    void quit();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    struct Invocation {
        QStringList arguments;
        QString cwd = QDir::currentPath();
        //! Whether the command line comes from another invocation
        bool remote = false;
        std::function<void(const QString &text, bool error)> print;
        //! Keep the invocation alive until this comparison is closed
        std::function<void(MeldDoc *)> holdTab;
        QByteArray activationToken;
        QByteArray startupId;
    };

    int handleInvocation(const Invocation &invocation);
    MeldWindow *newWindow();
    int localError(const Invocation &invocation, const QString &message);
    void activate(MeldWindow *window, const Invocation &invocation);

    QList<QPointer<MeldWindow>> windows_;
    QPointer<MeldWindow> lastFocused_;
    QPointer<PreferencesDialog> preferences_;
    SingleInstanceServer *server_ = nullptr;
    QString programName_ = QStringLiteral("meld-qt");
};

} // namespace meld
