#include "msgarea.h"

#include "icons.h"

#include <QApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStyle>
#include <QVBoxLayout>

namespace meld {

MsgArea::MsgArea(const QString &primary, const QString &secondary, const QString &iconName, QWidget *parent)
    : QFrame(parent)
{
    setObjectName(QStringLiteral("meldMsgArea"));
    setFrameShape(QFrame::NoFrame);
    setAutoFillBackground(true);
    QPalette pal = palette();
    const QColor highlight = pal.color(QPalette::Highlight);
    const QColor window = pal.color(QPalette::Window);
    // A subtle tint of the highlight colour, like GtkInfoBar's info style
    QColor background = QColor::fromRgbF(window.redF() * 0.82f + highlight.redF() * 0.18f,
                                         window.greenF() * 0.82f + highlight.greenF() * 0.18f,
                                         window.blueF() * 0.82f + highlight.blueF() * 0.18f);
    pal.setColor(QPalette::Window, background);
    setPalette(pal);

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(12, 8, 8, 8);
    layout->setSpacing(8);

    if (!iconName.isEmpty()) {
        auto *image = new QLabel(this);
        const int size = style()->pixelMetric(QStyle::PM_MessageBoxIconSize) * 2 / 3;
        image->setPixmap(icons::icon(iconName).pixmap(size, size));
        image->setAlignment(Qt::AlignCenter);
        layout->addWidget(image, 0, Qt::AlignVCenter);
    }

    auto *textBox = new QVBoxLayout;
    textBox->setSpacing(6);
    auto *primaryLabel = new QLabel(this);
    primaryLabel->setTextFormat(Qt::PlainText);
    primaryLabel->setText(primary);
    QFont bold = primaryLabel->font();
    bold.setBold(true);
    primaryLabel->setFont(bold);
    primaryLabel->setWordWrap(true);
    primaryLabel->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    primaryLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    textBox->addWidget(primaryLabel);
    if (!secondary.isEmpty()) {
        auto *secondaryLabel = new QLabel(this);
        secondaryLabel->setTextFormat(Qt::PlainText);
        secondaryLabel->setText(secondary);
        QFont small = secondaryLabel->font();
        small.setPointSizeF(small.pointSizeF() * 0.9);
        secondaryLabel->setFont(small);
        secondaryLabel->setWordWrap(true);
        secondaryLabel->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        textBox->addWidget(secondaryLabel);
    }
    textBox->addStretch();
    layout->addLayout(textBox, 1);

    // The action buttons are stacked vertically, because some button labels
    // are long and look odd next to each other.
    buttonBox_ = new QVBoxLayout;
    buttonBox_->setSpacing(4);
    layout->addLayout(buttonBox_);
}

QPushButton *MsgArea::addButton(const QString &text, int responseId)
{
    auto *button = new QPushButton(text, this);
    buttonBox_->addWidget(button);
    connect(button, &QPushButton::clicked, this, [this, responseId]() { Q_EMIT response(responseId); });
    return button;
}

MsgAreaController::MsgAreaController(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    hide();
}

void MsgAreaController::clear()
{
    if (msgarea_) {
        msgarea_->hide();
        msgarea_->deleteLater();
        msgarea_ = nullptr;
    }
    msgId_.reset();
    hide();
}

MsgArea *MsgAreaController::newFromTextAndIcon(const QString &primary, const QString &secondary,
                                               const QString &iconName)
{
    clear();
    auto *msgarea = new MsgArea(primary, secondary, iconName, this);
    layout()->addWidget(msgarea);
    msgarea_ = msgarea;
    show();
    return msgarea;
}

MsgArea *MsgAreaController::addDismissableMsg(const QString &iconName, const QString &primary,
                                              const QString &secondary, const QList<MsgAreaController *> &closePanes)
{
    MsgArea *msgarea = newFromTextAndIcon(primary, secondary, iconName);
    msgarea->addButton(tr("Hi&de"), Response::Close);
    QList<QPointer<MsgAreaController>> panes;
    for (MsgAreaController *pane : closePanes)
        panes.append(pane);
    connect(msgarea, &MsgArea::response, this, [this, panes](int) {
        if (!panes.isEmpty()) {
            for (const auto &pane : panes) {
                if (pane)
                    pane->clear();
            }
        } else {
            clear();
        }
    });
    return msgarea;
}

MsgArea *MsgAreaController::addActionMsg(const QString &iconName, const QString &primary, const QString &secondary,
                                         const QString &actionLabel, std::function<void()> callback)
{
    MsgArea *msgarea = newFromTextAndIcon(primary, secondary, iconName);
    msgarea->addButton(actionLabel, Response::Accept);
    msgarea->addButton(tr("Hi&de"), Response::Close);
    connect(msgarea, &MsgArea::response, this, [this, callback = std::move(callback)](int responseId) {
        clear();
        if (responseId == Response::Accept && callback)
            callback();
    });
    return msgarea;
}

} // namespace meld
