#include "trash.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QPushButton>

namespace meld {

namespace {

struct Tr {
    Q_DECLARE_TR_FUNCTIONS(meld::trash)
};

} // namespace

TrashResult trashOrConfirm(const QString &path, QWidget *parent, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return TrashResult::Failed;
    };

    if (QFile::moveToTrash(path))
        return TrashResult::Removed;

    // Trashing usually fails because the target is on a (probably network)
    // mount without a trash location, rather than due to an underlying
    // problem, so we offer to delete the file instead.
    const QFileInfo info(path);
    if (info.isDir())
        return fail(Tr::tr("Deleting remote folders is not supported"));
    if (!info.isFile())
        return fail(Tr::tr("Not a file or directory"));

    QMessageBox box(QMessageBox::Question, Tr::tr("Delete Permanently?"), Tr::tr("Delete Permanently?"), QMessageBox::NoButton,
                    parent);
    box.setInformativeText(
        Tr::tr("“%1” can’t be put in the trash. Do you want to delete it permanently?").arg(QDir::toNativeSeparators(path)));
    QPushButton *cancel = box.addButton(Tr::tr("&Cancel"), QMessageBox::RejectRole);
    QPushButton *remove = box.addButton(Tr::tr("&Delete Permanently"), QMessageBox::DestructiveRole);
    box.setDefaultButton(cancel);
    box.exec();
    if (box.clickedButton() != remove)
        return TrashResult::Cancelled;

    QFile file(path);
    if (!file.remove())
        return fail(file.errorString());
    return TrashResult::Removed;
}

} // namespace meld
