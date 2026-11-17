TEMPLATE = app
TARGET = meld-qt
CONFIG += windows

include(../../meld-qt.pri)
include(../ui/ui.pri)

SOURCES += main.cpp

# The icon that Explorer, the taskbar and shortcuts show for the program, and
# its version information, which qmake writes a resource file for, as CMake
# does with meld-qt.rc.in (tools/generate_windows_icon.py writes the icon).
# VERSION has to be numeric, so it's the project's, not MELD_VERSION.
win32 {
    RC_ICONS = meld-qt.ico
    VERSION = $$MELD_PROJECT_VERSION
    QMAKE_TARGET_PRODUCT = Meld Qt
    QMAKE_TARGET_COMPANY = Meld Qt
    QMAKE_TARGET_DESCRIPTION = Meld Qt - Visual diff and merge tool
    QMAKE_TARGET_COPYRIGHT = Copyright (c) 2026 Alexander R.
}

macx: target.path = $$PREFIX
else: target.path = $$PREFIX/bin
INSTALLS += target

# CONFIG+=meld_deploy_qt: "make install" runs windeployqt on the installed
# program, which copies the libraries, plugins and translations it needs next
# to it, as CMake's MELD_DEPLOY_QT does
win32:meld_deploy_qt {
    qtPrepareTool(MELD_WINDEPLOYQT, windeployqt)
    meld_deploy.path = $$target.path
    meld_deploy.depends = install_target
    # INSTALL_ROOT goes after the drive, as qmake puts it in the paths it
    # installs to
    meld_deploy_exe = $$target.path/$${TARGET}.exe
    contains(meld_deploy_exe, "^[A-Za-z]:.*"): \
        meld_deploy_exe = $$replace(meld_deploy_exe, "^([A-Za-z]:)", "\\1$(INSTALL_ROOT)")
    else: \
        meld_deploy_exe = $(INSTALL_ROOT)$$meld_deploy_exe
    meld_deploy.commands = $$MELD_WINDEPLOYQT --force $$shell_quote($$shell_path($$meld_deploy_exe))
    INSTALLS += meld_deploy
}
