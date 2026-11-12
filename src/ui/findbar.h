#pragma once

#include <QList>
#include <QPointer>
#include <QWidget>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QToolButton;

namespace meld {

class SourceView;
struct SearchHighlight;

class FindBar : public QWidget {
    Q_OBJECT

public:
    explicit FindBar(QWidget *parent = nullptr);

    void setTextViews(const QList<SourceView *> &views);
    //! Set the active text view that find/replace acts on
    void setTextView(SourceView *view);
    void startFind(SourceView *view, bool replace, const QString &text);
    void startFindNext(SourceView *view);
    void startFindPrevious(SourceView *view);
    void hideBar();

    bool replaceMode() const { return replaceMode_; }
    void setReplaceMode(bool replace);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

private:
    SearchHighlight settings() const;
    void updateHighlights();
    void updateMatchState();
    bool findText(int startOffset = 1, bool backwards = false);
    void replaceOne();
    void replaceAll();

    QList<QPointer<SourceView>> views_;
    QPointer<SourceView> view_;
    QLineEdit *findEntry_;
    QLineEdit *replaceEntry_;
    QToolButton *previousButton_;
    QToolButton *nextButton_;
    QToolButton *toggleReplaceButton_;
    QPushButton *replaceButton_;
    QPushButton *replaceAllButton_;
    QCheckBox *matchCase_;
    QCheckBox *wholeWord_;
    QCheckBox *regex_;
    QWidget *wrapBox_;
    bool replaceMode_ = false;
    static QString cachedSearch_;
};

} // namespace meld
