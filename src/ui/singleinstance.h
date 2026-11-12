// Single-instance mode: forwarding command lines to a running Meld
//
// The original is a GApplication, which always forwards a new invocation's
// command line to the running instance over D-Bus. Here this is optional:
// the "single-instance" setting, and the --single-instance option. It's
// built (MELD_SINGLE_INSTANCE) with one of two implementations:
//
//  * shared memory (the default), which needs only Qt Core. Instances in
//    single-instance mode poll a shared memory segment for command lines,
//    and others don't use it at all, so --newtab can only reach an
//    instance in single-instance mode.
//  * local sockets from Qt Network (MELD_SINGLE_INSTANCE_NETWORK). Every
//    instance listens, so --newtab also reaches instances started without
//    single-instance mode.

#pragma once

#include <QByteArray>
#include <QObject>
#include <QStringList>

#include <functional>
#include <memory>
#include <optional>

namespace meld {

//! A command line forwarded by another invocation (GApplicationCommandLine)
class RemoteCommandLine : public QObject {
    Q_OBJECT

public:
    struct Request {
        QStringList arguments;
        //! The invocation's working directory, for relative paths
        QString cwd;
        //! The invocation's window activation tokens (XDG_ACTIVATION_TOKEN
        //! and DESKTOP_STARTUP_ID), which allow raising windows for it
        QByteArray activationToken;
        QByteArray startupId;
    };
    //! Sends an encoded message to the invocation
    using Sender = std::function<void(const QByteArray &message)>;

    RemoteCommandLine(const Request &request, Sender send, std::function<void()> close, QObject *parent = nullptr);
    ~RemoteCommandLine() override;

    QStringList arguments() const { return request_.arguments; }
    QString cwd() const { return request_.cwd; }
    QByteArray activationToken() const { return request_.activationToken; }
    QByteArray startupId() const { return request_.startupId; }

    //! Print to the invocation's standard output or error
    void print(const QString &text, bool error);
    //! End the invocation with an exit status
    void finish(int status);
    bool isFinished() const { return finished_; }

    //! For implementations: the invocation went
    void notifyDisconnected();

Q_SIGNALS:
    //! The invocation ended (e.g., was interrupted) before it was finished
    void disconnected();

private:
    Request request_;
    Sender send_;
    std::function<void()> close_;
    bool finished_ = false;
};

//! Receives the command lines of later invocations
class SingleInstanceServer : public QObject {
    Q_OBJECT

public:
    explicit SingleInstanceServer(QObject *parent = nullptr);
    ~SingleInstanceServer() override;

    /*!
     * Whether every instance receives command lines, rather than only
     * those in single-instance mode; then --newtab reaches any instance
     */
    static const bool ReceivesWithoutSingleInstanceMode;

    /*!
     * The name of the running instance's socket or shared memory: per user
     * and display. MELD_SINGLE_INSTANCE_NAME overrides it (e.g., for tests).
     */
    static QString instanceName();

    //! Start receiving command lines; fails if another instance does
    bool listen(const QString &name = instanceName());
    bool isListening() const;

Q_SIGNALS:
    //! The receiver owns the command line, and must finish it
    void commandLineReceived(meld::RemoteCommandLine *commandLine);

private:
    struct Private;
    std::unique_ptr<Private> d_;
};

/*!
 * Forward a command line to the running instance, and relay what it prints
 *
 * Returns the invocation's exit status, which comes once its comparison is
 * closed, or nullopt if no instance is running. `print` receives the
 * output; by default it's written to the standard output and error.
 */
std::optional<int> forwardCommandLine(const QStringList &arguments, const QString &cwd,
                                      const std::function<void(const QString &text, bool error)> &print = {},
                                      const QString &name = SingleInstanceServer::instanceName());

} // namespace meld
