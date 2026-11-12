#include "findbar.h"

#include "icons.h"
#include "sourceview.h"

#include <QCheckBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QTextBlock>
#include <QTextDocument>
#include <QToolButton>

namespace meld {

QString FindBar::cachedSearch_;

FindBar::FindBar(QWidget *parent)
    : QWidget(parent)
{
    auto *grid = new QGridLayout(this);
    grid->setContentsMargins(6, 6, 6, 6);
    grid->setHorizontalSpacing(6);
    grid->setVerticalSpacing(6);

    findEntry_ = new QLineEdit(this);
    findEntry_->setPlaceholderText(tr("Find"));
    findEntry_->setClearButtonEnabled(true);
    findEntry_->addAction(icons::icon(QStringLiteral("edit-find-symbolic")), QLineEdit::LeadingPosition);
    findEntry_->setMinimumWidth(findEntry_->fontMetrics().averageCharWidth() * 32);
    findEntry_->installEventFilter(this);
    grid->addWidget(findEntry_, 0, 0);

    auto *navigation = new QWidget(this);
    auto *navigationLayout = new QHBoxLayout(navigation);
    navigationLayout->setContentsMargins(0, 0, 0, 0);
    navigationLayout->setSpacing(0);
    previousButton_ = new QToolButton(navigation);
    previousButton_->setIcon(icons::icon(QStringLiteral("go-up-symbolic")));
    previousButton_->setToolTip(tr("Previous Result"));
    nextButton_ = new QToolButton(navigation);
    nextButton_->setIcon(icons::icon(QStringLiteral("go-down-symbolic")));
    nextButton_->setToolTip(tr("Next Result"));
    navigationLayout->addWidget(previousButton_);
    navigationLayout->addWidget(nextButton_);
    grid->addWidget(navigation, 0, 1);

    toggleReplaceButton_ = new QToolButton(this);
    toggleReplaceButton_->setIcon(icons::icon(QStringLiteral("edit-find-replace-symbolic")));
    toggleReplaceButton_->setToolTip(tr("Switch between Find and Find-and-Replace"));
    grid->addWidget(toggleReplaceButton_, 0, 2);

    auto *options = new QWidget(this);
    auto *optionsLayout = new QHBoxLayout(options);
    optionsLayout->setContentsMargins(0, 0, 0, 0);
    optionsLayout->setSpacing(6);
    matchCase_ = new QCheckBox(tr("&Match case"), options);
    wholeWord_ = new QCheckBox(tr("Who&le word"), options);
    regex_ = new QCheckBox(tr("Regular e&xpression"), options);
    for (QCheckBox *box : {matchCase_, wholeWord_, regex_}) {
        box->setFocusPolicy(Qt::TabFocus);
        optionsLayout->addWidget(box);
    }
    grid->addWidget(options, 0, 3);

    wrapBox_ = new QWidget(this);
    auto *wrapLayout = new QHBoxLayout(wrapBox_);
    wrapLayout->setContentsMargins(0, 0, 0, 0);
    wrapLayout->setSpacing(6);
    auto *wrapImage = new QLabel(wrapBox_);
    wrapImage->setPixmap(icons::icon(QStringLiteral("view-wrapped-symbolic")).pixmap(16, 16));
    wrapLayout->addWidget(wrapImage);
    wrapLayout->addWidget(new QLabel(tr("Wrapped"), wrapBox_));
    wrapBox_->hide();
    grid->addWidget(wrapBox_, 0, 4);
    grid->setColumnStretch(5, 1);

    replaceEntry_ = new QLineEdit(this);
    replaceEntry_->setPlaceholderText(tr("Replace"));
    replaceEntry_->addAction(icons::icon(QStringLiteral("edit-find-replace-symbolic")), QLineEdit::LeadingPosition);
    replaceEntry_->installEventFilter(this);
    grid->addWidget(replaceEntry_, 1, 0);
    replaceButton_ = new QPushButton(tr("&Replace"), this);
    replaceButton_->setFocusPolicy(Qt::TabFocus);
    grid->addWidget(replaceButton_, 1, 1);
    replaceAllButton_ = new QPushButton(tr("Replace &All"), this);
    replaceAllButton_->setFocusPolicy(Qt::TabFocus);
    grid->addWidget(replaceAllButton_, 1, 2);

    connect(previousButton_, &QToolButton::clicked, this, [this]() { findText(1, true); });
    connect(nextButton_, &QToolButton::clicked, this, [this]() { findText(); });
    connect(toggleReplaceButton_, &QToolButton::clicked, this, [this]() { setReplaceMode(!replaceMode_); });
    connect(findEntry_, &QLineEdit::textChanged, this, [this](const QString &text) {
        cachedSearch_ = text;
        updateHighlights();
        findText(0);
        updateMatchState();
    });
    connect(findEntry_, &QLineEdit::returnPressed, this, [this]() { findText(); });
    connect(replaceEntry_, &QLineEdit::returnPressed, this, &FindBar::replaceOne);
    connect(replaceButton_, &QPushButton::clicked, this, &FindBar::replaceOne);
    connect(replaceAllButton_, &QPushButton::clicked, this, &FindBar::replaceAll);
    for (QCheckBox *box : {matchCase_, wholeWord_, regex_}) {
        connect(box, &QCheckBox::toggled, this, [this]() {
            updateHighlights();
            updateMatchState();
        });
    }

    setReplaceMode(false);
}

void FindBar::paintEvent(QPaintEvent *event)
{
    QWidget::paintEvent(event);
    QPainter painter(this);
    painter.setPen(palette().color(QPalette::Mid));
    painter.drawLine(0, 0, width(), 0);
}

bool FindBar::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::ShortcutOverride) {
        // Escape belongs to the find bar while it has focus
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        if (keyEvent->key() == Qt::Key_Escape) {
            event->accept();
            return false;
        }
    }
    if (event->type() == QEvent::KeyPress) {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        if (keyEvent->key() == Qt::Key_Escape) {
            hideBar();
            return true;
        }
        if (watched == findEntry_ && (keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter)
            && (keyEvent->modifiers() & Qt::ShiftModifier)) {
            findText(1, true);
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void FindBar::setReplaceMode(bool replace)
{
    replaceMode_ = replace;
    replaceEntry_->setVisible(replace);
    replaceButton_->setVisible(replace);
    replaceAllButton_->setVisible(replace);
}

void FindBar::setTextViews(const QList<SourceView *> &views)
{
    views_.clear();
    for (SourceView *view : views)
        views_.append(view);
}

void FindBar::setTextView(SourceView *view)
{
    view_ = view;
    updateMatchState();
}

SearchHighlight FindBar::settings() const
{
    SearchHighlight highlight;
    highlight.text = findEntry_->text();
    highlight.caseSensitive = matchCase_->isChecked();
    highlight.wholeWord = wholeWord_->isChecked();
    highlight.regex = regex_->isChecked();
    return highlight;
}

void FindBar::updateHighlights()
{
    const SearchHighlight highlight = isVisible() ? settings() : SearchHighlight();
    for (const auto &view : std::as_const(views_)) {
        if (view)
            view->setSearchHighlight(highlight);
    }
}

void FindBar::hideBar()
{
    view_ = nullptr;
    wrapBox_->hide();
    hide();
    updateHighlights();
}

void FindBar::updateMatchState()
{
    // We only show an error state for the active pane results, even if
    // there are matches in other panes.
    bool noMatches = false;
    const SearchHighlight search = settings();
    if (view_ && !search.text.isEmpty()) {
        QTextDocument::FindFlags flags;
        if (search.caseSensitive)
            flags |= QTextDocument::FindCaseSensitively;
        if (search.wholeWord)
            flags |= QTextDocument::FindWholeWords;
        QTextCursor found;
        if (search.regex) {
            QRegularExpression regex(search.text, search.caseSensitive
                                                      ? QRegularExpression::NoPatternOption
                                                      : QRegularExpression::CaseInsensitiveOption);
            found = regex.isValid() ? view_->document()->find(regex, 0, flags) : QTextCursor();
        } else {
            found = view_->document()->find(search.text, 0, flags);
        }
        noMatches = found.isNull();
    }
    QPalette pal = findEntry_->palette();
    if (noMatches) {
        pal.setColor(QPalette::Base, QColor(0xf6, 0xd3, 0xd3));
        pal.setColor(QPalette::Text, QColor(0x80, 0x00, 0x00));
    } else {
        pal = palette();
    }
    findEntry_->setPalette(pal);
}

void FindBar::startFind(SourceView *view, bool replace, const QString &text)
{
    setReplaceMode(replace);
    setTextView(view);

    const QString prefilledText = !text.isEmpty() ? text : cachedSearch_;
    if (!prefilledText.isEmpty()) {
        const QSignalBlocker blocker(findEntry_);
        findEntry_->setText(prefilledText);
        cachedSearch_ = prefilledText;
    }
    show();
    findEntry_->setFocus();
    findEntry_->selectAll();
    updateHighlights();
    updateMatchState();
}

void FindBar::startFindNext(SourceView *view)
{
    setTextView(view);
    findText();
}

void FindBar::startFindPrevious(SourceView *view)
{
    setTextView(view);
    findText(1, true);
}

bool FindBar::findText(int startOffset, bool backwards)
{
    if (!view_)
        return false;
    const SearchHighlight search = settings();
    if (search.text.isEmpty()) {
        wrapBox_->hide();
        return false;
    }

    QTextDocument *doc = view_->document();
    QTextCursor insert = view_->textCursor();
    // The insert position is the start of any current match
    int position = std::min(insert.position(), insert.anchor());
    if (!backwards)
        position = std::min(position + startOffset, doc->characterCount() - 1);

    QTextDocument::FindFlags flags;
    if (search.caseSensitive)
        flags |= QTextDocument::FindCaseSensitively;
    if (search.wholeWord)
        flags |= QTextDocument::FindWholeWords;
    if (backwards)
        flags |= QTextDocument::FindBackward;

    QRegularExpression regex;
    if (search.regex) {
        regex = QRegularExpression(search.text, search.caseSensitive ? QRegularExpression::NoPatternOption
                                                                     : QRegularExpression::CaseInsensitiveOption);
        if (!regex.isValid())
            return false;
    }
    const auto find = [&](int from) {
        return search.regex ? doc->find(regex, from, flags) : doc->find(search.text, from, flags);
    };

    wrapBox_->hide();
    QTextCursor match = find(position);
    bool wrapped = false;
    if (match.isNull()) {
        match = find(backwards ? doc->characterCount() - 1 : 0);
        wrapped = !match.isNull();
    }

    if (!match.isNull()) {
        wrapBox_->setVisible(wrapped);
        QTextCursor cursor(doc);
        cursor.setPosition(match.selectionEnd());
        cursor.setPosition(match.selectionStart(), QTextCursor::KeepAnchor);
        view_->setTextCursor(cursor);
        view_->scrollToLineCentered(cursor.blockNumber());
        return true;
    }
    QTextCursor cursor = view_->textCursor();
    cursor.setPosition(cursor.position());
    view_->setTextCursor(cursor);
    return false;
}

void FindBar::replaceOne()
{
    if (!view_ || !view_->editable())
        return;
    const QTextCursor oldSelection = view_->textCursor();
    const bool matched = findText(0);
    const QTextCursor newSelection = view_->textCursor();
    // Only replace if there is an already-selected match at the cursor
    if (matched && oldSelection.hasSelection() && oldSelection.selectionStart() == newSelection.selectionStart()
        && oldSelection.selectionEnd() == newSelection.selectionEnd()) {
        QTextCursor cursor = newSelection;
        QString replacement = replaceEntry_->text();
        if (regex_->isChecked()) {
            const QRegularExpression regex(findEntry_->text(), matchCase_->isChecked()
                                                                   ? QRegularExpression::NoPatternOption
                                                                   : QRegularExpression::CaseInsensitiveOption);
            replacement = cursor.selectedText().replace(regex, replacement);
        }
        cursor.beginEditBlock();
        cursor.insertText(replacement);
        cursor.endEditBlock();
        view_->setTextCursor(cursor);
        findText(0);
    }
}

void FindBar::replaceAll()
{
    if (!view_ || !view_->editable())
        return;
    const SearchHighlight search = settings();
    if (search.text.isEmpty())
        return;
    QTextDocument *doc = view_->document();
    QTextDocument::FindFlags flags;
    if (search.caseSensitive)
        flags |= QTextDocument::FindCaseSensitively;
    if (search.wholeWord)
        flags |= QTextDocument::FindWholeWords;
    const QRegularExpression regex(search.text, search.caseSensitive ? QRegularExpression::NoPatternOption
                                                                     : QRegularExpression::CaseInsensitiveOption);

    const int savedPosition = view_->textCursor().position();
    QTextCursor editCursor(doc);
    editCursor.beginEditBlock();
    QTextCursor match = search.regex ? doc->find(regex, 0, flags) : doc->find(search.text, 0, flags);
    while (!match.isNull()) {
        QString replacement = replaceEntry_->text();
        if (search.regex)
            replacement = match.selectedText().replace(regex, replacement);
        match.insertText(replacement);
        const int next = match.position();
        match = search.regex ? doc->find(regex, next, flags) : doc->find(search.text, next, flags);
        if (!match.isNull() && match.selectionEnd() == next && match.selectionStart() == next)
            break;
    }
    editCursor.endEditBlock();
    QTextCursor cursor(doc);
    cursor.setPosition(std::min(savedPosition, doc->characterCount() - 1));
    view_->setTextCursor(cursor);
    view_->scrollToLineCentered(cursor.blockNumber());
}

} // namespace meld
