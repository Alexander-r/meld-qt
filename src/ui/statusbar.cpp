#include "statusbar.h"

#include "bufferselectors.h"

#include "core/textfile.h"
#include "icons.h"

#include <QActionGroup>
#include <QHBoxLayout>
#include <QIntValidator>
#include <QLineEdit>
#include <QMenu>
#include <QWidgetAction>

namespace meld {

StatusMenuButton::StatusMenuButton(QWidget *parent)
    : QToolButton(parent)
{
    setAutoRaise(true);
    setPopupMode(QToolButton::InstantPopup);
    setToolButtonStyle(Qt::ToolButtonTextOnly);
    setFocusPolicy(Qt::NoFocus);
}

void StatusMenuButton::setLabelWidth(int chars)
{
    labelChars_ = chars;
    updateGeometry();
}

QSize StatusMenuButton::sizeHint() const
{
    QSize hint = QToolButton::sizeHint();
    if (labelChars_ > 0) {
        const int minimum = fontMetrics().averageCharWidth() * labelChars_ + 24;
        hint.setWidth(std::max(hint.width(), minimum));
    }
    return hint;
}

StatusBar::StatusBar(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(4, 0, 4, 0);
    layout->setSpacing(6);
    layout->addStretch();

    displayButton_ = new StatusMenuButton(this);
    //: This is the status bar label for a group of settings, such as text
    //: wrapping, show line numbers, whitespace, etc.
    displayButton_->setText(tr("Display"));
    displayButton_->setMenu(new QMenu(displayButton_));
    layout->addWidget(displayButton_);

    encodingButton_ = new StatusMenuButton(this);
    encodingMenu_ = new QMenu(encodingButton_);
    auto *encodingGroup = new QActionGroup(encodingMenu_);
    for (const QByteArray &encoding : selectableEncodings()) {
        QAction *action = encodingMenu_->addAction(encodingDisplayName(encoding));
        action->setCheckable(true);
        action->setData(encoding);
        encodingGroup->addAction(action);
    }
    connect(encodingMenu_, &QMenu::aboutToShow, this, [this]() {
        for (QAction *action : encodingMenu_->actions())
            action->setChecked(action->data().toByteArray().compare(encoding_, Qt::CaseInsensitive) == 0);
    });
    connect(encodingMenu_, &QMenu::triggered, this, [this](QAction *action) {
        const QByteArray encoding = action->data().toByteArray();
        if (encoding.compare(encoding_, Qt::CaseInsensitive) != 0)
            Q_EMIT encodingChanged(encoding);
    });
    encodingButton_->setMenu(encodingMenu_);
    layout->addWidget(encodingButton_);

    languageButton_ = new StatusMenuButton(this);
    languageMenu_ = new QMenu(languageButton_);
    languageSelector_ = new SourceLangSelector(languageMenu_);
    auto *languageAction = new QWidgetAction(languageMenu_);
    languageAction->setDefaultWidget(languageSelector_);
    languageMenu_->addAction(languageAction);
    connect(languageMenu_, &QMenu::aboutToShow, this,
            [this]() { languageSelector_->selectValue(languageId_.isEmpty() ? QVariant() : QVariant(languageId_)); });
    connect(languageMenu_, &QMenu::aboutToShow, languageSelector_,
            [this]() { languageSelector_->entry()->setFocus(); }, Qt::QueuedConnection);
    connect(languageSelector_, &SourceLangSelector::valueSelected, this, [this](const QVariant &value) {
        languageMenu_->close();
        const QString id = value.toString();
        if (id == languageId_)
            return;
        setLanguage(id);
        Q_EMIT languageSelected(id);
    });
    languageButton_->setMenu(languageMenu_);
    layout->addWidget(languageButton_);

    // Note that we're receiving one-based line numbers from the user and
    // storing and emitting zero-base line numbers.
    lineButton_ = new StatusMenuButton(this);
    lineMenu_ = new QMenu(lineButton_);
    auto *entryContainer = new QWidget(lineMenu_);
    auto *entryLayout = new QHBoxLayout(entryContainer);
    entryLayout->setContentsMargins(6, 6, 6, 6);
    lineEntry_ = new QLineEdit(entryContainer);
    lineEntry_->setToolTip(tr("Line you want to move the cursor to"));
    lineEntry_->addAction(icons::icon(QStringLiteral("go-jump-symbolic")), QLineEdit::LeadingPosition);
    lineEntry_->setValidator(new QIntValidator(1, 1 << 30, lineEntry_));
    entryLayout->addWidget(lineEntry_);
    auto *entryAction = new QWidgetAction(lineMenu_);
    entryAction->setDefaultWidget(entryContainer);
    lineMenu_->addAction(entryAction);
    connect(lineMenu_, &QMenu::aboutToShow, this, [this]() {
        lineEntry_->setText(QString::number(line_ + 1));
        lineEntry_->selectAll();
    });
    connect(lineMenu_, &QMenu::aboutToShow, lineEntry_, [this]() { lineEntry_->setFocus(); },
            Qt::QueuedConnection);
    connect(lineEntry_, &QLineEdit::textEdited, this, [this](const QString &text) {
        bool ok = false;
        const int line = text.toInt(&ok);
        if (ok)
            Q_EMIT goToLine(std::max(0, line - 1));
    });
    connect(lineEntry_, &QLineEdit::returnPressed, this, [this]() {
        bool ok = false;
        const int line = lineEntry_->text().toInt(&ok);
        if (ok)
            Q_EMIT goToLine(std::max(0, line - 1));
        lineMenu_->close();
    });
    lineButton_->setMenu(lineMenu_);
    // Set a label width to avoid other widgets moving on cursor change
    lineButton_->setLabelWidth(int(tr("Ln %1, Col %2").arg(1000).arg(100).size()));
    layout->addWidget(lineButton_);

    setCursorPosition(0, 0);
    setEncoding(QByteArrayLiteral("UTF-8"));
    setLanguage(QString());
}

void StatusBar::setDisplayActions(const QList<QAction *> &actions)
{
    QMenu *menu = displayButton_->menu();
    menu->clear();
    for (QAction *action : actions)
        menu->addAction(action);
}

void StatusBar::setCursorPosition(int line, int column)
{
    line_ = line;
    // Abbreviation for line, column so that it will fit in the status bar
    lineButton_->setText(tr("Ln %1, Col %2").arg(line + 1).arg(column + 1));
}

void StatusBar::setEncoding(const QByteArray &encoding)
{
    encoding_ = encoding;
    encodingButton_->setText(encodingDisplayName(encoding));
}

void StatusBar::setLanguage(const QString &languageId)
{
    languageId_ = languageId;
    languageButton_->setText(SourceLangSelector::valueLabel(languageId));
}

void StatusBar::startGoToLine()
{
    lineButton_->showMenu();
}

} // namespace meld
