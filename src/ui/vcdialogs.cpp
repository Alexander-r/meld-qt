#include "vcdialogs.h"

#include "core/misc.h"
#include "settings.h"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QTextBlock>
#include <QVBoxLayout>

namespace meld {

namespace {

constexpr int MinItemLength = 3;
constexpr int HistoryLength = 10;

QLabel *groupTitle(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    QFont font = label->font();
    font.setBold(true);
    label->setFont(font);
    return label;
}

} // namespace

// ---------------------------------------------------------------------------
// CommitMessageEdit

CommitMessageEdit::CommitMessageEdit(QWidget *parent)
    : QPlainTextEdit(parent)
{
    MeldSettings *settings = MeldSettings::instance();
    setFont(settings->font());
    setWordWrapMode(QTextOption::WordWrap);
    insertSpaces_ = settings->boolValue(QStringLiteral("insert-spaces-instead-of-tabs"));
    indentWidth_ = std::max(1, settings->intValue(QStringLiteral("indent-width")));
    setTabStopDistance(QFontMetricsF(font()).horizontalAdvance(QLatin1Char(' ')) * indentWidth_);
}

void CommitMessageEdit::setRightMargin(bool show, int position)
{
    showRightMargin_ = show;
    rightMarginPosition_ = position;
    viewport()->update();
}

void CommitMessageEdit::paintEvent(QPaintEvent *event)
{
    QPlainTextEdit::paintEvent(event);
    if (!showRightMargin_)
        return;

    // The margin, as GtkSourceView draws it: a line at the margin column,
    // with the area beyond it shaded
    const qreal x = contentOffset().x() + document()->documentMargin()
        + QFontMetricsF(font()).horizontalAdvance(QLatin1Char('x')) * rightMarginPosition_;
    QPainter painter(viewport());
    QColor shade = palette().color(QPalette::Text);
    shade.setAlphaF(0.05f);
    painter.fillRect(QRectF(x, 0, viewport()->width() - x, viewport()->height()), shade);
    QColor line = palette().color(QPalette::Text);
    line.setAlphaF(0.25f);
    painter.setPen(line);
    painter.drawLine(QLineF(x, 0, x, viewport()->height()));
}

void CommitMessageEdit::keyPressEvent(QKeyEvent *event)
{
    if (insertSpaces_ && event->key() == Qt::Key_Tab && event->modifiers() == Qt::NoModifier) {
        QTextCursor cursor = textCursor();
        const int column = cursor.position() - cursor.block().position();
        cursor.insertText(QString(indentWidth_ - column % indentWidth_, QLatin1Char(' ')));
        return;
    }
    QPlainTextEdit::keyPressEvent(event);
}

// ---------------------------------------------------------------------------
// HistoryCombo

HistoryCombo::HistoryCombo(const QString &historyId, QWidget *parent)
    : QComboBox(parent)
    , historyId_(historyId)
{
    setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    setMinimumContentsLength(20);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    load();
}

void HistoryCombo::prependHistory(const QString &text)
{
    if (text.size() <= MinItemLength)
        return;

    const int existing = findData(text);
    if (existing >= 0) {
        removeItem(existing);
    } else {
        while (count() > HistoryLength - 2)
            removeItem(count() - 1);
    }
    insertItem(0, text.section(QLatin1Char('\n'), 0, 0), text);
    setCurrentIndex(-1);
    save();
}

QString HistoryCombo::historyText(int index) const
{
    return itemData(index).toString();
}

void HistoryCombo::load()
{
    clear();
    const QStringList messages = MeldSettings::instance()->stringListValue(historyId_);
    for (const QString &message : messages.mid(0, HistoryLength - 1))
        addItem(message.section(QLatin1Char('\n'), 0, 0), message);
    setCurrentIndex(-1);
}

void HistoryCombo::save()
{
    QStringList messages;
    for (int i = 0; i < count(); ++i)
        messages.append(historyText(i));
    MeldSettings::instance()->setValue(historyId_, messages);
}

// ---------------------------------------------------------------------------
// CommitDialog

CommitDialog::CommitDialog(QWidget *parent)
    : QDialog(parent)
{
    MeldSettings *settings = MeldSettings::instance();
    setWindowTitle(tr("Commit Files"));
    auto *layout = new QVBoxLayout(this);
    layout->setSpacing(6);

    auto *heading = new QLabel(tr("Commit Files"), this);
    QFont headingFont = heading->font();
    headingFont.setBold(true);
    headingFont.setPointSizeF(headingFont.pointSizeF() * 1.2);
    heading->setFont(headingFont);
    heading->setAlignment(Qt::AlignHCenter);
    layout->addWidget(heading);

    layout->addWidget(groupTitle(tr("Changed Files"), this));
    changedFiles_ = new QListWidget(this);
    changedFiles_->setSelectionMode(QAbstractItemView::NoSelection);
    changedFiles_->setFocusPolicy(Qt::NoFocus);
    layout->addWidget(changedFiles_);

    layout->addSpacing(6);
    layout->addWidget(groupTitle(tr("Log Message"), this));
    textview_ = new CommitMessageEdit(this);
    textview_->setRightMargin(settings->boolValue(QStringLiteral("vc-show-commit-margin")),
                              settings->intValue(QStringLiteral("vc-commit-margin")));
    breakCommitMessage_ = settings->boolValue(QStringLiteral("vc-break-commit-message"));
    // Try and make the editor wide enough for a standard 80-character
    // commit message.
    const int charWidth = QFontMetrics(textview_->font()).averageCharWidth();
    textview_->setMinimumSize(80 * charWidth + 2 * textview_->frameWidth() + 8, 200);
    layout->addWidget(textview_, 1);

    auto *previous = new QHBoxLayout;
    previous->setSpacing(12);
    previous->addWidget(new QLabel(tr("Previous logs:"), this));
    previousEntry_ = new HistoryCombo(QStringLiteral("commit-message-history"), this);
    previousEntry_->setFocusPolicy(Qt::NoFocus);
    connect(previousEntry_, &QComboBox::activated, this, [this](int index) {
        if (index != -1)
            textview_->setPlainText(previousEntry_->historyText(index));
    });
    previous->addWidget(previousEntry_, 1);
    layout->addLayout(previous);

    auto *buttons = new QDialogButtonBox(this);
    buttons->addButton(tr("&Cancel"), QDialogButtonBox::RejectRole);
    QPushButton *commit = buttons->addButton(tr("Co&mmit"), QDialogButtonBox::AcceptRole);
    commit->setDefault(true);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    textview_->setFocus();
}

void CommitDialog::setChangedFiles(const QStringList &files)
{
    changedFiles_->clear();
    changedFiles_->addItems(files);
    // As tall as the list, up to a limit, as the original's natural height
    int rowHeight = changedFiles_->sizeHintForRow(0);
    if (rowHeight <= 0)
        rowHeight = changedFiles_->fontMetrics().height() + 4;
    const int rows = std::max<int>(1, int(changedFiles_->count()));
    changedFiles_->setFixedHeight(std::min(200, rows * rowHeight + 2 * changedFiles_->frameWidth() + 4));
}

QStringList CommitDialog::changedFiles() const
{
    QStringList files;
    for (int i = 0; i < changedFiles_->count(); ++i)
        files.append(changedFiles_->item(i)->text());
    return files;
}

void CommitDialog::setMessage(const QString &message)
{
    if (message.isEmpty())
        return;
    textview_->setPlainText(message);
    textview_->moveCursor(QTextCursor::Start);
}

QString CommitDialog::message() const
{
    QString message = textview_->toPlainText();
    // This is a dependent option because of the margin column
    if (textview_->showRightMargin() && breakCommitMessage_) {
        QStringList paragraphs = message.split(QStringLiteral("\n\n"));
        for (QString &paragraph : paragraphs)
            paragraph = textWrapFill(paragraph, textview_->rightMarginPosition());
        message = paragraphs.join(QStringLiteral("\n\n"));
    }
    return message;
}

void CommitDialog::accept()
{
    const QString text = message();
    if (!text.trimmed().isEmpty())
        previousEntry_->prependHistory(text);
    QDialog::accept();
    Q_EMIT commitRequested(text);
}

} // namespace meld
