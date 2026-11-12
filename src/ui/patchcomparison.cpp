#include "patchcomparison.h"

#include "core/patch.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace meld {

namespace {

//! tr() for the code here that isn't in a QObject class
struct Tr {
    Q_DECLARE_TR_FUNCTIONS(meld::PatchComparison)
};

VirtualFile::Content contentFrom(const patch::ApplyResult &result, const QString &name, const QString &patchFile)
{
    VirtualFile::Content content;
    const QString patchName = QFileInfo(patchFile).fileName();
    if (!result.text) {
        content.errorTitle = Tr::tr("The patch “%1” can’t be applied to “%2”.").arg(patchName, name);
        content.error = result.error;
        return content;
    }
    content.data = result.text;
    content.notes = result.notes;
    if (result.deleted)
        content.notes.prepend(Tr::tr("The patch deletes this file."));
    if (!content.notes.isEmpty())
        content.noteTitle = Tr::tr("The patch “%1” was applied to “%2”.").arg(patchName, name);
    return content;
}

} // namespace

QString patchedLabel(const QString &name, const QString &patchFile)
{
    return Tr::tr("%1 with %2 applied").arg(name, QFileInfo(patchFile).fileName());
}

QString patchComparisonTabLabel(const QString &file, const QString &patchFile)
{
    return QStringLiteral("%1 — %2").arg(QFileInfo(file).fileName(), QFileInfo(patchFile).fileName());
}

VirtualFile patchedFile(const QString &file, const QString &patchFile)
{
    VirtualFile result;
    const QString name = QFileInfo(file).fileName();
    result.label = patchedLabel(name, patchFile);
    result.load = [file, patchFile, name]() {
        VirtualFile::Content content;
        QString error;
        const std::optional<patch::Patch> patch = patch::parseFile(patchFile, &error);
        if (!patch) {
            content.errorTitle = Tr::tr("The patch “%1” can’t be read.").arg(QDir::toNativeSeparators(patchFile));
            content.error = error;
            return content;
        }
        const patch::FilePatch *changes = patch::findFilePatch(*patch, file, &error);
        if (!changes) {
            content.errorTitle = Tr::tr("The patch “%1” can’t be applied to “%2”.").arg(QFileInfo(patchFile).fileName(), name);
            content.error = error;
            return content;
        }
        QByteArray original;
        QFile handle(file);
        if (handle.exists()) {
            if (!handle.open(QIODevice::ReadOnly)) {
                content.errorTitle = Tr::tr("There was a problem opening the file “%1”.").arg(QDir::toNativeSeparators(file));
                content.error = handle.errorString();
                return content;
            }
            original = handle.readAll();
        }
        return contentFrom(patch::apply(original, *changes), name, patchFile);
    };
    return result;
}

VirtualFile patchedFile(const std::shared_ptr<const patch::PatchedFolder> &folder, const QString &path)
{
    VirtualFile result;
    result.label = patchedLabel(path, folder->patchFile());
    result.load = [folder, path]() { return contentFrom(folder->content(path), path, folder->patchFile()); };
    return result;
}

} // namespace meld
