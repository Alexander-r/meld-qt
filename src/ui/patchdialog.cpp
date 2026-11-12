#include "patchdialog.h"

#include "core/syntax/highlighter.h"
#include "core/syntax/language.h"

#include "core/unifieddiff.h"
#include "style.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QSaveFile>
#include <QVBoxLayout>

namespace meld {

PatchDialog::PatchDialog(const QStringList &texts, const QStringList &labels, int numPanes, QWidget *parent)
    : QDialog(parent)
    , texts_(texts)
    , labels_(labels)
{
    setWindowTitle(tr("Format as Patch"));
    auto *layout = new QVBoxLayout(this);
    layout->setSpacing(6);

    auto *sideLabel = new QLabel(tr("Use differences between:"), this);
    layout->addWidget(sideLabel);
    auto *sideBox = new QWidget(this);
    auto *sideLayout = new QVBoxLayout(sideBox);
    sideLayout->setContentsMargins(18, 0, 0, 0);
    leftRadio_ = new QRadioButton(tr("Left and middle panes"), sideBox);
    leftRadio_->setChecked(true);
    rightRadio_ = new QRadioButton(tr("Middle and right panes"), sideBox);
    sideLayout->addWidget(leftRadio_);
    sideLayout->addWidget(rightRadio_);
    layout->addWidget(sideBox);
    if (numPanes < 3) {
        sideLabel->hide();
        sideBox->hide();
    }

    reverseCheck_ = new QCheckBox(tr("&Reverse patch direction"), this);
    layout->addWidget(reverseCheck_);

    textview_ = new QPlainTextEdit(this);
    textview_->setReadOnly(true);
    textview_->setLineWrapMode(QPlainTextEdit::NoWrap);
    textview_->setFont(editorFont());
    textview_->setMinimumSize(600, 400);
    layout->addWidget(textview_, 1);

    // The original highlights the patch with GtkSourceView's default
    // scheme, which follows the light or dark style; Meld's current scheme
    // does too.
    textview_->setPalette(schemeTextPalette(currentStyleScheme()));
    highlighter_ = new syntax::SyntaxHighlighter(textview_->document());
    highlighter_->setBaseFont(editorFont());
    highlighter_->setStyleScheme(currentStyleScheme());
    highlighter_->setLanguage(syntax::LanguageManager::instance().guessLanguageForMimeType(QStringLiteral("text/x-diff")));
    highlighter_->setHighlightSyntax(true);

    auto *buttons = new QDialogButtonBox(this);
    QPushButton *copyButton = buttons->addButton(tr("Copy to Clipboard"), QDialogButtonBox::ActionRole);
    buttons->addButton(tr("&Cancel"), QDialogButtonBox::RejectRole);
    QPushButton *saveButton = buttons->addButton(tr("Save Patch"), QDialogButtonBox::AcceptRole);
    saveButton->setDefault(true);
    layout->addWidget(buttons);

    connect(leftRadio_, &QRadioButton::toggled, this, &PatchDialog::updatePatch);
    connect(reverseCheck_, &QCheckBox::toggled, this, &PatchDialog::updatePatch);
    connect(copyButton, &QPushButton::clicked, this, [this]() {
        QApplication::clipboard()->setText(patchText());
        accept();
    });
    connect(saveButton, &QPushButton::clicked, this, &PatchDialog::savePatch);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    updatePatch();
}

QString PatchDialog::patchText() const
{
    return textview_->toPlainText();
}

void PatchDialog::updatePatch()
{
    int from = leftRadio_->isChecked() ? 0 : 1;
    int to = from + 1;
    if (reverseCheck_->isChecked())
        std::swap(from, to);
    textview_->setPlainText(formatPatch(texts_, labels_, from, to));
    // Patches are short, and plain text edits don't highlight lazily
    highlighter_->highlightAll();
}

void PatchDialog::savePatch()
{
    const QString patch = patchText();
    QWidget *owner = parentWidget();
    auto *dialog = new QFileDialog(owner, tr("Save Patch"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setAcceptMode(QFileDialog::AcceptSave);
    connect(dialog, &QFileDialog::fileSelected, owner, [owner, patch](const QString &path) {
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(patch.toUtf8()) < 0 || !file.commit()) {
            QMessageBox::critical(owner, QObject::tr("Could not save file %1.").arg(path),
                                  QObject::tr("Couldn’t save file due to:\n%1").arg(file.errorString()));
        }
    });
    dialog->open();
    accept();
}

} // namespace meld
