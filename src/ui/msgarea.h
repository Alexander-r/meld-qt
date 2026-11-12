#pragma once

#include <QFrame>
#include <QList>
#include <QPointer>

#include <functional>
#include <optional>

class QPushButton;
class QVBoxLayout;

namespace meld {

//! GtkResponseType values used by message areas
namespace Response {
constexpr int Accept = -3;
constexpr int Ok = -5;
constexpr int Cancel = -6;
constexpr int Close = -7;
} // namespace Response

//! An information bar shown at the top of a comparison pane (GtkInfoBar)
class MsgArea : public QFrame {
    Q_OBJECT

public:
    MsgArea(const QString &primary, const QString &secondary, const QString &iconName, QWidget *parent = nullptr);

    QPushButton *addButton(const QString &text, int responseId);

Q_SIGNALS:
    void response(int responseId);

private:
    QVBoxLayout *buttonBox_;
};

class MsgAreaController : public QWidget {
    Q_OBJECT

public:
    explicit MsgAreaController(QWidget *parent = nullptr);

    bool hasMessage() const { return !msgarea_.isNull(); }
    std::optional<int> msgId() const { return msgId_; }
    void setMsgId(std::optional<int> id) { msgId_ = id; }
    void clear();

    MsgArea *newFromTextAndIcon(const QString &primary, const QString &secondary = QString(),
                                const QString &iconName = QString());
    MsgArea *addDismissableMsg(const QString &iconName, const QString &primary, const QString &secondary,
                               const QList<MsgAreaController *> &closePanes = {});
    MsgArea *addActionMsg(const QString &iconName, const QString &primary, const QString &secondary,
                          const QString &actionLabel, std::function<void()> callback);

private:
    QPointer<MsgArea> msgarea_;
    std::optional<int> msgId_;
};

} // namespace meld
