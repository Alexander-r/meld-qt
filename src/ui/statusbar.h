#pragma once

#include <QByteArray>
#include <QToolButton>
#include <QWidget>

class QAction;
class QLineEdit;
class QMenu;

namespace meld {

class SourceLangSelector;

//! Compact, flat menu button with arrow indicator for use in a status bar
class StatusMenuButton : public QToolButton {
    Q_OBJECT

public:
    explicit StatusMenuButton(QWidget *parent = nullptr);
    void setLabelWidth(int chars);
    QSize sizeHint() const override;

private:
    int labelChars_ = 0;
};

class StatusBar : public QWidget {
    Q_OBJECT

public:
    explicit StatusBar(QWidget *parent = nullptr);

    //! Actions shown in the "Display" menu, shared by all panes
    void setDisplayActions(const QList<QAction *> &actions);
    void setCursorPosition(int line, int column);
    void setEncoding(const QByteArray &encoding);
    //! The language shown in the highlighting selector; empty for plain text
    void setLanguage(const QString &languageId);
    QString language() const { return languageId_; }
    //! Open the go-to-line popup
    void startGoToLine();

Q_SIGNALS:
    //! Zero-based line number
    void goToLine(int line);
    void encodingChanged(const QByteArray &encoding);
    //! A language was chosen from the highlighting selector
    void languageSelected(const QString &languageId);

private:
    StatusMenuButton *displayButton_;
    StatusMenuButton *encodingButton_;
    StatusMenuButton *languageButton_;
    StatusMenuButton *lineButton_;
    QMenu *encodingMenu_;
    QMenu *languageMenu_;
    SourceLangSelector *languageSelector_;
    QString languageId_;
    QMenu *lineMenu_;
    QLineEdit *lineEntry_;
    QByteArray encoding_;
    int line_ = 0;
};

} // namespace meld
