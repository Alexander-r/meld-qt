// Single-instance mode with local sockets (Qt Network)

#include "singleinstance_p.h"

#include <QDir>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLoggingCategory>
#include <QPointer>
#include <QStandardPaths>

namespace meld {

Q_LOGGING_CATEGORY(lcSingleInstance, "meld.singleinstance")

using namespace singleinstance;

namespace {

constexpr int ConnectTimeout = 2000;

//! A socket only the user can reach: in the runtime folder, unless the name
//! is a path; on Windows, a named pipe, which is per session
QString socketPath(const QString &name)
{
#ifndef Q_OS_WIN
    if (!QDir::isAbsolutePath(name)) {
        const QString folder = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
        if (!folder.isEmpty())
            return QDir(folder).filePath(name);
    }
#endif
    return name;
}

} // namespace

// Every instance listens, so --newtab reaches instances started without
// single-instance mode
const bool SingleInstanceServer::ReceivesWithoutSingleInstanceMode = true;

struct SingleInstanceServer::Private {
    QLocalServer *server = nullptr;
};

SingleInstanceServer::SingleInstanceServer(QObject *parent)
    : QObject(parent)
    , d_(std::make_unique<Private>())
{
    d_->server = new QLocalServer(this);
    d_->server->setSocketOptions(QLocalServer::UserAccessOption);
    connect(d_->server, &QLocalServer::newConnection, this, [this]() {
        while (QLocalSocket *socket = d_->server->nextPendingConnection()) {
            // The request comes first; then a command line takes the socket
            auto buffer = std::make_shared<QByteArray>();
            const auto read = [this, socket, buffer]() {
                *buffer += socket->readAll();
                const std::optional<QJsonObject> message = takeMessage(*buffer);
                if (!message)
                    return;
                disconnect(socket, &QLocalSocket::readyRead, this, nullptr);
                QByteArray answer;
                const std::optional<RemoteCommandLine::Request> request = parseRequest(*message, &answer);
                if (!request) {
                    socket->write(answer);
                    socket->disconnectFromServer();
                    return;
                }
                QPointer<QLocalSocket> guard(socket);
                auto *commandLine = new RemoteCommandLine(
                    *request,
                    [guard](const QByteArray &data) {
                        if (guard && guard->state() == QLocalSocket::ConnectedState) {
                            guard->write(data);
                            guard->flush();
                        }
                    },
                    [guard]() {
                        if (guard)
                            guard->disconnectFromServer();
                    },
                    this);
                socket->setParent(commandLine);
                connect(socket, &QLocalSocket::disconnected, commandLine, &RemoteCommandLine::notifyDisconnected);
                Q_EMIT commandLineReceived(commandLine);
            };
            connect(socket, &QLocalSocket::readyRead, this, read);
            connect(socket, &QLocalSocket::disconnected, socket, [socket]() {
                // Unless a command line took it over
                if (!qobject_cast<RemoteCommandLine *>(socket->parent()))
                    socket->deleteLater();
            });
            if (socket->bytesAvailable())
                read();
        }
    });
}

SingleInstanceServer::~SingleInstanceServer() = default;

bool SingleInstanceServer::listen(const QString &name)
{
    const QString path = socketPath(name);
    // A running instance answers. This is checked first, as with access
    // options Qt replaces existing sockets rather than failing.
    QLocalSocket probe;
    probe.connectToServer(path);
    if (probe.waitForConnected(ConnectTimeout))
        return false;
    if (d_->server->listen(path))
        return true;
    // A socket left by an instance that ended without removing it
    QLocalServer::removeServer(path);
    if (d_->server->listen(path))
        return true;
    qCWarning(lcSingleInstance) << "Couldn't listen for other invocations:" << d_->server->errorString();
    return false;
}

bool SingleInstanceServer::isListening() const
{
    return d_->server->isListening();
}

std::optional<int> forwardCommandLine(const QStringList &arguments, const QString &cwd,
                                      const std::function<void(const QString &, bool)> &print, const QString &name)
{
    QLocalSocket socket;
    socket.connectToServer(socketPath(name));
    if (!socket.waitForConnected(ConnectTimeout))
        return std::nullopt;
    socket.write(request(arguments, cwd));
    if (!socket.waitForBytesWritten(ConnectTimeout))
        return std::nullopt;

    // Relay the output until the invocation ends, which may be when its
    // comparison is closed
    QByteArray buffer;
    while (true) {
        while (const std::optional<QJsonObject> message = takeMessage(buffer)) {
            if (const std::optional<int> status = handleAnswer(*message, print))
                return status;
        }
        if (!socket.bytesAvailable() && !socket.waitForReadyRead(-1) && !socket.bytesAvailable()
            && socket.state() != QLocalSocket::ConnectedState) {
            break;
        }
        buffer += socket.readAll();
    }
    // The running instance ended without answering
    return 1;
}

} // namespace meld
