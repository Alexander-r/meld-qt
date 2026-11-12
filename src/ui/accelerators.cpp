#include "accelerators.h"

namespace meld {

const QList<QPair<QString, QStringList>> &viewAccelerators()
{
    static const QList<QPair<QString, QStringList>> accelerators{
        {"app.quit", {"Ctrl+Q"}},
        {"app.preferences", {"Ctrl+,"}},
        {"view.find", {"Ctrl+F"}},
        {"view.find-next", {"Ctrl+G", "F3"}},
        {"view.find-previous", {"Ctrl+Shift+G", "Shift+F3"}},
        {"view.find-replace", {"Ctrl+H"}},
        {"view.go-to-line", {"Ctrl+I"}},
        // We bind the next-change-shortcut action instead of the next-change
        // action because the latter is used for buttons and so we set its
        // sensitivity correctly. The -shortcut variant doesn't have
        // sensitivity set, and so we get alert bells when trying to move
        // past the last/first change.
        {"view.next-change-shortcut", {"Alt+Down", "Alt+Num+Down", "Ctrl+D"}},
        {"view.next-pane", {"Alt+PgDown"}},
        {"view.open-external", {"Ctrl+Shift+O"}},
        // See view.next-change-shortcut
        {"view.previous-change-shortcut", {"Alt+Up", "Alt+Num+Up", "Ctrl+E"}},
        {"view.previous-pane", {"Alt+PgUp"}},
        {"view.redo", {"Ctrl+Shift+Z"}},
        {"view.refresh", {"Ctrl+R", "F5"}},
        {"view.save", {"Ctrl+S"}},
        {"view.save-all", {"Ctrl+Shift+L"}},
        {"view.save-as", {"Ctrl+Shift+S"}},
        {"view.undo", {"Ctrl+Z"}},
        {"win.close", {"Ctrl+W"}},
        {"win.gear-menu", {"F10"}},
        {"win.fullscreen", {"F11"}},
        {"win.new-tab", {"Ctrl+N"}},
        {"win.stop", {"Esc"}},
        // Provided by GtkApplication and AdwTabView in the original
        {"win.show-help-overlay", {"Ctrl+?"}},
        {"win.previous-tab", {"Ctrl+Alt+PgUp"}},
        {"win.next-tab", {"Ctrl+Alt+PgDown"}},
        {"win.move-tab-left", {"Ctrl+Alt+Shift+PgUp"}},
        {"win.move-tab-right", {"Ctrl+Alt+Shift+PgDown"}},
        // Shared bindings for per-view filter menu buttons
        {"view.vc-filter", {"F8"}},
        {"view.folder-filter", {"F8"}},
        {"view.text-filter", {"F8"}},
        // File comparison actions
        {"view.file-previous-conflict", {"Ctrl+J"}},
        {"view.file-next-conflict", {"Ctrl+K"}},
        {"view.file-push-left", {"Alt+Left"}},
        {"view.file-push-right", {"Alt+Right"}},
        {"view.file-pull-left", {"Alt+Shift+Right"}},
        {"view.file-pull-right", {"Alt+Shift+Left"}},
        {"view.file-copy-left-up", {"Alt+["}},
        {"view.file-copy-right-up", {"Alt+]"}},
        {"view.file-copy-left-down", {"Alt+;"}},
        {"view.file-copy-right-down", {"Alt+'"}},
        {"view.file-delete", {"Alt+Del", "Alt+Num+Del"}},
        {"view.show-overview-map", {"F9"}},
        {"view.zoom-in", {"Ctrl++", "Ctrl+=", "Ctrl+Num++"}},
        {"view.zoom-out", {"Ctrl+-", "Ctrl+Num+-"}},
        {"view.zoom-reset", {"Ctrl+0", "Ctrl+Num+0"}},
        // Folder comparison actions
        {"view.folder-compare", {"Return"}},
        {"view.folder-copy-left", {"Alt+Left"}},
        {"view.folder-copy-right", {"Alt+Right"}},
        {"view.folder-delete", {"Del"}},
        // Version control actions
        {"view.vc-commit", {"Ctrl+M"}},
        {"view.vc-console-visible", {"F9"}},
        // Swap the two panes
        {"view.swap-2-panes", {"Alt+\\"}},
    };
    return accelerators;
}

QList<QKeySequence> shortcutsForAction(const QString &action)
{
    QList<QKeySequence> result;
    for (const auto &[name, keys] : viewAccelerators()) {
        if (name != action)
            continue;
        for (const QString &key : keys)
            result.append(QKeySequence::fromString(key, QKeySequence::PortableText));
    }
    return result;
}

} // namespace meld
