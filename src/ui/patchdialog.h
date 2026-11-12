#pragma once

#include <QDialog>
#include <QStringList>

class QCheckBox;
class QPlainTextEdit;
class QRadioButton;

namespace meld {

namespace syntax {
class SyntaxHighlighter;
} // namespace syntax

class PatchDialog : public QDialog {
    Q_OBJECT

public:
    //! `texts` and `labels` hold all three buffers, as in the original
    PatchDialog(const QStringList &texts, const QStringList &labels, int numPanes, QWidget *parent = nullptr);

    QString patchText() const;

private:
    void updatePatch();
    void savePatch();

    QStringList texts_;
    QStringList labels_;
    QRadioButton *leftRadio_;
    QRadioButton *rightRadio_;
    QCheckBox *reverseCheck_;
    QPlainTextEdit *textview_;
    syntax::SyntaxHighlighter *highlighter_;
};

} // namespace meld
