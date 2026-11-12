// Single-instance mode: the messages that implementations exchange

#pragma once

#include "singleinstance.h"

#include <QJsonObject>

#include <optional>

namespace meld::singleinstance {

// Messages are JSON objects, one per line. An invocation sends
// {"version": ..., "arguments": [...], "cwd": ..., "activation-token": ...,
// "startup-id": ...}; the running instance answers with
// {"print": text, "error": bool} messages and finally {"exit": status}.
constexpr int ProtocolVersion = 1;

QByteArray encode(const QJsonObject &message);
//! Take the first complete message from a buffer
std::optional<QJsonObject> takeMessage(QByteArray &buffer);

//! This invocation's request
QByteArray request(const QStringList &arguments, const QString &cwd);
//! A received request, or nullopt (and an answer for the invocation) if it
//! can't be handled
std::optional<RemoteCommandLine::Request> parseRequest(const QJsonObject &message, QByteArray *answer);

/*!
 * Handle an answer from the running instance: print it, or return the exit
 * status it ends with
 */
std::optional<int> handleAnswer(const QJsonObject &message,
                                const std::function<void(const QString &text, bool error)> &print);

} // namespace meld::singleinstance
