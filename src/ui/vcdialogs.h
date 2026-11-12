#pragma once

#include <QComboBox>
#include <QDialog>
#include <QPlainTextEdit>

class QListWidget;

namespace meld {

//! A plain text editor for commit messages, with an optional right margin
class CommitMessageEdit : public QPlainTextEdit {
    Q_OBJECT

public:
    explicit CommitMessageEdit(QWidget *parent = nullptr);

    bool showRightMargin() const { return showRightMargin_; }
    int rightMarginPosition() const { return rightMarginPosition_; }
    void setRightMargin(bool show, int position);

protected:
    void paintEvent(QPaintEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    bool showRightMargin_ = false;
    int rightMarginPosition_ = 72;
    bool insertSpaces_ = false;
    int indentWidth_ = 8;
};

//! A combo box of previously entered texts, kept in the settings
class HistoryCombo : public QComboBox {
    Q_OBJECT

public:
    explicit HistoryCombo(const QString &historyId, QWidget *parent = nullptr);

    //! Add a text to the start of the history
    void prependHistory(const QString &text);
    //! The full text of a history item
    QString historyText(int index) const;

private:
    void load();
    void save();

    QString historyId_;
};

class CommitDialog : public QDialog {
    Q_OBJECT

public:
    explicit CommitDialog(QWidget *parent = nullptr);

    void setChangedFiles(const QStringList &files);
    QStringList changedFiles() const;
    //! Prefill the log message, as given by the version control system
    void setMessage(const QString &message);
    //! The log message, broken at the right margin if that's enabled
    QString message() const;

    CommitMessageEdit *messageEdit() const { return textview_; }
    HistoryCombo *previousLogs() const { return previousEntry_; }

    void accept() override;

Q_SIGNALS:
    void commitRequested(const QString &message);

private:
    QListWidget *changedFiles_;
    CommitMessageEdit *textview_;
    HistoryCombo *previousEntry_;
    bool breakCommitMessage_ = false;
};

} // namespace meld
