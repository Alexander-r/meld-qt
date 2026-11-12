# Widgets: the comparison tabs, their custom views and the main window
TEMPLATE = lib
CONFIG += staticlib
TARGET = meldui

include(../../meld-qt.pri)
include(ui.pri)

SOURCES += \
    accelerators.cpp \
    actiongutter.cpp \
    bufferselectors.cpp \
    chunkmap.cpp \
    dirdiff.cpp \
    dirdifftree.cpp \
    externalhelpers.cpp \
    filediff.cpp \
    filediff_layout.cpp \
    findbar.cpp \
    icons.cpp \
    inlinematcher.cpp \
    linkmap.cpp \
    meldapp.cpp \
    melddoc.cpp \
    meldwindow.cpp \
    msgarea.cpp \
    conflictcomparison.cpp \
    newdifftab.cpp \
    patchcomparison.cpp \
    patchdialog.cpp \
    pathlabel.cpp \
    preferences.cpp \
    recent.cpp \
    settings.cpp \
    sourceview.cpp \
    statusbar.cpp \
    style.cpp \
    syncpoints.cpp \
    trash.cpp \
    vcdialogs.cpp \
    vcview.cpp

HEADERS += \
    accelerators.h \
    actiongutter.h \
    bufferselectors.h \
    chunkmap.h \
    dirdiff.h \
    dirdifftree.h \
    externalhelpers.h \
    filediff.h \
    findbar.h \
    icons.h \
    inlinematcher.h \
    linkmap.h \
    meldapp.h \
    melddoc.h \
    meldwindow.h \
    msgarea.h \
    conflictcomparison.h \
    newdifftab.h \
    patchcomparison.h \
    patchdialog.h \
    pathlabel.h \
    preferences.h \
    recent.h \
    settings.h \
    sourceview.h \
    statusbar.h \
    style.h \
    syncpoints.h \
    trash.h \
    vcdialogs.h \
    vcview.h

# Single-instance mode, with shared memory or Qt Network's local sockets
!meld_no_single_instance {
    SOURCES += singleinstance.cpp
    HEADERS += singleinstance.h singleinstance_p.h
    meld_single_instance_network: SOURCES += singleinstance_localsocket.cpp
    else: SOURCES += singleinstance_sharedmemory.cpp
}

# Meld's own icons (the merge action arrows and the application icon)
meld_icons.prefix = /icons
meld_icons.base = $$PWD/../resources/icons
meld_icons.files = \
    ../resources/icons/meld-change-apply-left.png \
    ../resources/icons/meld-change-apply-right.png \
    ../resources/icons/meld-change-copy.png \
    ../resources/icons/meld-change-delete.png \
    ../resources/icons/meld-change-apply-left-dark.png \
    ../resources/icons/meld-change-apply-right-dark.png \
    ../resources/icons/meld-change-copy-dark.png \
    ../resources/icons/meld-change-delete-dark.png \
    ../resources/icons/meld-qt.svg \
    ../resources/icons/vc-pull-symbolic.svg \
    ../resources/icons/vc-push-symbolic.svg
RESOURCES += meld_icons
