# Desktop integration (freedesktop.org): the application's desktop file, so
# that it's in application menus and can open files, its icons, and the MIME
# type of saved comparisons (*.meldcmp).
TEMPLATE = aux
CONFIG -= qt

include(../meld-qt.pri)

# TARGET in src/app/app.pro
meld_desktop_in = $$cat(Meld-Qt.desktop.in, lines)
meld_desktop = $$replace(meld_desktop_in, @MELD_EXECUTABLE@, meld-qt)
meld_desktop_file = $$OUT_PWD/Meld-Qt.desktop
write_file($$meld_desktop_file, meld_desktop)|error()

desktop_entry.path = $$PREFIX/share/applications
desktop_entry.files = $$meld_desktop_file
desktop_entry.CONFIG += no_check_exist

mime_type.path = $$PREFIX/share/mime/packages
mime_type.files = Meld-Qt.xml

icon.path = $$PREFIX/share/icons/hicolor/scalable/apps
icon.files = icons/hicolor/scalable/apps/Meld-Qt.svg

symbolic_icon.path = $$PREFIX/share/icons/hicolor/symbolic/apps
symbolic_icon.files = icons/hicolor/symbolic/apps/Meld-Qt-symbolic.svg

INSTALLS += desktop_entry mime_type icon symbolic_icon

# As GNOME's post-install step: the desktop, MIME and icon caches are
# updated, if their programs are installed, unless the files are being
# staged for a package (INSTALL_ROOT), whose installation updates them
update_caches.path = $$PREFIX/share
update_caches.extra = test -n \"$(INSTALL_ROOT)\" || sh $$shell_quote($$PWD/update-caches.sh) $$shell_quote($$PREFIX/share)
update_caches.depends = install_desktop_entry install_mime_type install_icon install_symbolic_icon
INSTALLS += update_caches

check.commands = ! command -v desktop-file-validate > /dev/null || desktop-file-validate $$shell_quote($$meld_desktop_file)
QMAKE_EXTRA_TARGETS += check

OTHER_FILES += Meld-Qt.desktop.in update-caches.sh
