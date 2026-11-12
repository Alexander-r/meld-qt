// Single-instance mode: what the implementations share

#include "singleinstance_p.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>

#include <cstdio>

namespace meld {

namespace singleinstance {

QByteArray encode(const QJsonObject &message)
{
    return QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
}

std::optional<QJsonObject> takeMessage(QByteArray &buffer)
{
    const qsizetype end = buffer.indexOf('\n');
    if (end < 0)
        return std::nullopt;
    const QByteArray line = buffer.left(end);
    buffer.remove(0, end + 1);
    return QJsonDocument::fromJson(line).object();
}

QByteArray request(const QStringList &arguments, const QString &cwd)
{
    QJsonObject message{
        {QStringLiteral("version"), ProtocolVersion},
        {QStringLiteral("arguments"), QJsonArray::fromStringList(arguments)},
        {QStringLiteral("cwd"), cwd},
    };
    // Window managers let the running instance raise its window with these
    if (const QString token = qEnvironmentVariable("XDG_ACTIVATION_TOKEN"); !token.isEmpty())
        message.insert(QStringLiteral("activation-token"), token);
    if (const QString id = qEnvironmentVariable("DESKTOP_STARTUP_ID"); !id.isEmpty())
        message.insert(QStringLiteral("startup-id"), id);
    return encode(message);
}

std::optional<RemoteCommandLine::Request> parseRequest(const QJsonObject &message, QByteArray *answer)
{
    if (message.value(QStringLiteral("version")).toInt() != ProtocolVersion) {
        *answer = encode({{QStringLiteral("print"),
                           QCoreApplication::translate("SingleInstanceServer",
                                                       "Meld Qt couldn’t open the comparison: a different version of "
                                                       "Meld Qt is running.\n")},
                          {QStringLiteral("error"), true}})
            + encode({{QStringLiteral("exit"), 1}});
        return std::nullopt;
    }
    RemoteCommandLine::Request request;
    for (const QJsonValue &argument : message.value(QStringLiteral("arguments")).toArray())
        request.arguments.append(argument.toString());
    request.cwd = message.value(QStringLiteral("cwd")).toString();
    request.activationToken = message.value(QStringLiteral("activation-token")).toString().toUtf8();
    request.startupId = message.value(QStringLiteral("startup-id")).toString().toUtf8();
    return request;
}

std::optional<int> handleAnswer(const QJsonObject &message,
                                const std::function<void(const QString &text, bool error)> &print)
{
    if (message.contains(QStringLiteral("exit")))
        return message.value(QStringLiteral("exit")).toInt();
    const QString text = message.value(QStringLiteral("print")).toString();
    const bool error = message.value(QStringLiteral("error")).toBool();
    if (print) {
        print(text, error);
    } else {
        const QByteArray bytes = text.toLocal8Bit();
        FILE *stream = error ? stderr : stdout;
        std::fwrite(bytes.constData(), 1, size_t(bytes.size()), stream);
        std::fflush(stream);
    }
    return std::nullopt;
}

} // namespace singleinstance

using namespace singleinstance;

// ---------------------------------------------------------------------------
// RemoteCommandLine

RemoteCommandLine::RemoteCommandLine(const Request &request, Sender send, std::function<void()> close,
                                     QObject *parent)
    : QObject(parent)
    , request_(request)
    , send_(std::move(send))
    , close_(std::move(close))
{
}

RemoteCommandLine::~RemoteCommandLine()
{
    if (!finished_)
        finish(0);
}

void RemoteCommandLine::print(const QString &text, bool error)
{
    if (!finished_ && send_)
        send_(encode({{QStringLiteral("print"), text}, {QStringLiteral("error"), error}}));
}

void RemoteCommandLine::finish(int status)
{
    if (finished_)
        return;
    finished_ = true;
    if (send_)
        send_(encode({{QStringLiteral("exit"), status}}));
    if (close_)
        close_();
}

void RemoteCommandLine::notifyDisconnected()
{
    if (!finished_)
        Q_EMIT disconnected();
}

// ---------------------------------------------------------------------------

QString SingleInstanceServer::instanceName()
{
    const QString override = qEnvironmentVariable("MELD_SINGLE_INSTANCE_NAME");
    if (!override.isEmpty())
        return override;
    // One instance per user and display, as with the session bus that the
    // original uses
    QString display = qEnvironmentVariable("WAYLAND_DISPLAY");
    if (display.isEmpty())
        display = qEnvironmentVariable("DISPLAY");
    QString user = qEnvironmentVariable("USER");
    if (user.isEmpty())
        user = qEnvironmentVariable("USERNAME");
    QString name = QStringLiteral("meld-qt-%1-%2").arg(user, display);
    name.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_.-]")), QStringLiteral("_"));
    return name;
}

} // namespace meld
