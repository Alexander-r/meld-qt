#include "newdifftab.h"

#include "icons.h"

#include <QCheckBox>
#include <QDir>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>

namespace meld {

NewDiffTab::NewDiffTab(QWidget *parent)
    : MeldDoc(parent)
{
    setTabLabel(tr("New comparison"), QString());
    lastFolder_ = QDir::homePath();

    auto *outer = new QVBoxLayout(this);
    outer->addStretch(1);
    auto *centered = new QHBoxLayout;
    outer->addLayout(centered);
    outer->addStretch(1);
    centered->addStretch(1);

    auto *content = new QWidget(this);
    content->setMaximumWidth(760);
    centered->addWidget(content, 3);
    centered->addStretch(1);

    auto *box = new QVBoxLayout(content);
    box->setSpacing(18);

    auto *title = new QLabel(tr("New comparison"), content);
    QFont titleFont = title->font();
    titleFont.setPointSizeF(titleFont.pointSizeF() * 2.5);
    title->setFont(titleFont);
    QPalette titlePalette = title->palette();
    QColor dim = titlePalette.color(QPalette::WindowText);
    dim.setAlphaF(0.5f);
    titlePalette.setColor(QPalette::WindowText, dim);
    title->setPalette(titlePalette);
    box->addWidget(title);

    auto *types = new QHBoxLayout;
    types->setSpacing(12);
    const struct {
        const char *icon;
        QString label;
    } typeSpecs[] = {
        {"document-new-symbolic", tr("File")},
        {"folder-new-symbolic", tr("Folder")},
        {"appointment-new-symbolic", tr("Version control")},
    };
    for (const auto &spec : typeSpecs) {
        auto *button = new QToolButton(content);
        button->setCheckable(true);
        button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        button->setIcon(icons::icon(QLatin1String(spec.icon)));
        button->setIconSize(QSize(48, 48));
        button->setText(spec.label);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        button->setMinimumHeight(110);
        QFont font = button->font();
        font.setPointSizeF(font.pointSizeF() * 1.2);
        button->setFont(font);
        connect(button, &QToolButton::toggled, this, [this, button](bool checked) { onTypeToggled(button, checked); });
        typeButtons_.append(button);
        types->addWidget(button, 1);
    }
    box->addLayout(types);

    choosers_ = new QStackedWidget(content);
    choosers_->addWidget(new QWidget(choosers_));

    const auto makeChooserPage = [this](const QStringList &labels, bool folder, QList<QPushButton *> &buttons,
                                        QCheckBox **threeWay, QCheckBox **patch) {
        auto *page = new QWidget(choosers_);
        auto *grid = new QGridLayout(page);
        grid->setContentsMargins(0, 0, 0, 0);
        grid->setHorizontalSpacing(12);
        grid->setVerticalSpacing(6);
        for (int i = 0; i < labels.size(); ++i) {
            auto *button = new QPushButton(labels.at(i), page);
            button->setProperty("defaultLabel", labels.at(i));
            connect(button, &QPushButton::clicked, this, [this, button, folder]() { chooseFile(button, folder); });
            grid->addWidget(button, 0, i);
            grid->setColumnStretch(i, 1);
            buttons.append(button);
        }
        buttons.at(2)->setEnabled(false);
        auto *check = new QCheckBox(tr("&3-way comparison"), page);
        connect(check, &QCheckBox::toggled, buttons.at(2), &QPushButton::setEnabled);
        grid->addWidget(check, 1, 2);
        *threeWay = check;

        // The second file can be a patch, to compare with the first as the
        // patch would change it
        auto *patchCheck = new QCheckBox(tr("Compare with a &patch"), page);
        patchCheck->setToolTip(folder ? tr("Compare the folder with how a patch would change it")
                                      : tr("Compare the file with how a patch would change it"));
        QPushButton *second = buttons.at(1);
        const QString secondLabel = labels.at(1);
        connect(patchCheck, &QCheckBox::toggled, this, [this, second, secondLabel, check](bool patch) {
            const QString label = patch ? tr("Select Patch File") : secondLabel;
            second->setProperty("defaultLabel", label);
            second->setProperty("patch", patch);
            second->setText(label);
            second->setToolTip(QString());
            values_.remove(second);
            if (patch)
                check->setChecked(false);
            check->setEnabled(!patch);
        });
        grid->addWidget(patchCheck, 1, 1);
        *patch = patchCheck;
        return page;
    };
    choosers_->addWidget(makeChooserPage({tr("Select First File"), tr("Select Second File"), tr("Select Third File")},
                                         false, fileChoosers_, &fileThreeWay_, &filePatch_));
    choosers_->addWidget(makeChooserPage(
        {tr("Select First Folder"), tr("Select Second Folder"), tr("Select Third Folder")}, true, dirChoosers_,
        &dirThreeWay_, &dirPatch_));
    auto *vcPage = new QWidget(choosers_);
    auto *vcGrid = new QGridLayout(vcPage);
    vcGrid->setContentsMargins(0, 0, 0, 0);
    vcChooser_ = new QPushButton(tr("Select Version-controlled Folder"), vcPage);
    vcChooser_->setProperty("defaultLabel", vcChooser_->text());
    connect(vcChooser_, &QPushButton::clicked, this, [this]() { chooseFile(vcChooser_, true); });
    vcGrid->addWidget(vcChooser_, 0, 0);
    vcGrid->setColumnStretch(1, 2);
    choosers_->addWidget(vcPage);
    box->addWidget(choosers_);

    auto *actions = new QHBoxLayout;
    blankButton_ = new QPushButton(tr("&Blank comparison"), content);
    blankButton_->setEnabled(false);
    compareButton_ = new QPushButton(tr("C&ompare"), content);
    compareButton_->setEnabled(false);
    compareButton_->setDefault(true);
    updateSuggestedStyle();
    actions->addWidget(blankButton_);
    actions->addStretch(1);
    actions->addWidget(compareButton_);
    box->addLayout(actions);

    connect(blankButton_, &QPushButton::clicked, this, &NewDiffTab::newBlank);
    connect(compareButton_, &QPushButton::clicked, this, &NewDiffTab::compare);
}

void NewDiffTab::updateSuggestedStyle()
{
    // Like GTK's suggested-action buttons, the button has the accent colour
    // when it can be used; disabled, it looks like any disabled button
    QPalette suggested;
    for (QPalette::ColorGroup group : {QPalette::Active, QPalette::Inactive}) {
        suggested.setColor(group, QPalette::Button, palette().color(group, QPalette::Highlight));
        suggested.setColor(group, QPalette::ButtonText, palette().color(group, QPalette::HighlightedText));
    }
    compareButton_->setPalette(suggested);
}

void NewDiffTab::changeEvent(QEvent *event)
{
    MeldDoc::changeEvent(event);
    if ((event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange) && compareButton_)
        updateSuggestedStyle();
}

QWidget *NewDiffTab::focusPane() const
{
    return typeButtons_.first();
}

void NewDiffTab::onTypeToggled(QToolButton *button, bool checked)
{
    if (!checked) {
        bool anyActive = false;
        for (QToolButton *b : std::as_const(typeButtons_))
            anyActive = anyActive || b->isChecked();
        if (!anyActive) {
            const QSignalBlocker blocker(button);
            button->setChecked(true);
        }
        return;
    }

    for (QToolButton *b : std::as_const(typeButtons_)) {
        if (b != button) {
            const QSignalBlocker blocker(b);
            b->setChecked(false);
        }
    }

    diffType_ = DiffType(typeButtons_.indexOf(button));
    choosers_->setCurrentIndex(int(diffType_) + 1);
    // FIXME: Add support for new blank for VcView
    blankButton_->setEnabled(diffType_ == DiffType::File || diffType_ == DiffType::Folder);
    compareButton_->setEnabled(true);
}

void NewDiffTab::chooseFile(QPushButton *button, bool folder)
{
    auto *dialog = new QFileDialog(window(), button->property("defaultLabel").toString(), lastFolder_);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    if (button->property("patch").toBool()) {
        dialog->setFileMode(QFileDialog::ExistingFile);
        dialog->setNameFilters({tr("Patches (*.patch *.diff)"), tr("All files (*)")});
    } else if (folder) {
        dialog->setFileMode(QFileDialog::Directory);
        dialog->setOption(QFileDialog::ShowDirsOnly);
    } else {
        dialog->setFileMode(QFileDialog::ExistingFile);
    }
    connect(dialog, &QFileDialog::fileSelected, this, [this, button](const QString &file) {
        if (file.isEmpty())
            return;
        const QFileInfo info(file);
        button->setText(info.fileName().isEmpty() ? file : info.fileName());
        button->setToolTip(QDir::toNativeSeparators(file));
        values_.insert(button, QDir::cleanPath(info.absoluteFilePath()));
        if (info.dir().exists())
            lastFolder_ = info.absolutePath();
    });
    dialog->open();
}

bool NewDiffTab::comparesWithPatch() const
{
    if (diffType_ == DiffType::File)
        return filePatch_->isChecked();
    if (diffType_ == DiffType::Folder)
        return dirPatch_->isChecked();
    return false;
}

int NewDiffTab::numPaths() const
{
    if (diffType_ == DiffType::File)
        return fileThreeWay_->isChecked() ? 3 : 2;
    if (diffType_ == DiffType::Folder)
        return dirThreeWay_->isChecked() ? 3 : 2;
    return 1;
}

void NewDiffTab::compare()
{
    QStringList paths;
    const QList<QPushButton *> &choosers = diffType_ == DiffType::File ? fileChoosers_ : dirChoosers_;
    if (comparesWithPatch()) {
        Q_EMIT patchDiffRequested(diffType_, values_.value(choosers.at(0)), values_.value(choosers.at(1)));
        return;
    }
    if (diffType_ == DiffType::Version) {
        paths.append(values_.value(vcChooser_));
    } else {
        for (int i = 0; i < numPaths(); ++i)
            paths.append(values_.value(choosers.at(i)));
    }
    Q_EMIT diffRequested(diffType_, paths);
}

void NewDiffTab::newBlank()
{
    QStringList paths;
    for (int i = 0; i < numPaths(); ++i)
        paths.append(QString());
    Q_EMIT diffRequested(diffType_, paths);
}

void NewDiffTab::requestClose()
{
    Q_EMIT closed(0);
}

} // namespace meld
