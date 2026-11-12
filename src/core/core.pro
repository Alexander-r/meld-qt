# Core library: the diff engine and document model, independent of widgets
TEMPLATE = lib
CONFIG += staticlib
TARGET = meldcore

include(../../meld-qt.pri)
include(core.pri)

SOURCES += \
    diffchunk.cpp \
    myers.cpp \
    linesource.cpp \
    diffutil.cpp \
    merge.cpp \
    misc.cpp \
    filters.cpp \
    iohelpers.cpp \
    archive.cpp \
    patch.cpp \
    conflictmarkers.cpp \
    filecompare.cpp \
    task.cpp \
    textfile.cpp \
    translations.cpp \
    meldbuffer.cpp \
    unifieddiff.cpp

HEADERS += \
    diffchunk.h \
    myers.h \
    linesource.h \
    diffutil.h \
    merge.h \
    misc.h \
    filters.h \
    iohelpers.h \
    archive.h \
    patch.h \
    conflictmarkers.h \
    filecompare.h \
    task.h \
    textfile.h \
    translations.h \
    meldbuffer.h \
    unifieddiff.h

# Version control support
SOURCES += \
    vc/vc.cpp \
    vc/git.cpp \
    vc/mercurial.cpp \
    vc/bzr.cpp \
    vc/svn.cpp \
    vc/darcs.cpp \
    vc/cvs.cpp

HEADERS += \
    vc/states.h \
    vc/vc.h \
    vc/vcutil.h \
    vc/plugins.h

# Syntax highlighting (ported from GtkSourceView)
SOURCES += \
    syntax/contextengine.cpp \
    syntax/highlighter.cpp \
    syntax/language.cpp \
    syntax/stylescheme.cpp

HEADERS += \
    syntax/contextengine.h \
    syntax/highlighter.h \
    syntax/language.h \
    syntax/stylescheme.h

# The language definitions and colour schemes for syntax highlighting, from
# GtkSourceView (see resources/language-specs/README). A new file needs qmake
# to be run again.
meld_syntax.prefix = /
meld_syntax.base = $$PWD/../resources
meld_syntax.files = \
    $$files($$PWD/../resources/language-specs/*.lang) \
    $$files($$PWD/../resources/styles/*.xml)
RESOURCES += meld_syntax

# The translations (in translations/ at the top), built in as resources in
# :/i18n; see translations/CMakeLists.txt. A new translation needs qmake to be
# run again.
EXTRA_TRANSLATIONS = $$files($$PWD/../../translations/meld-qt_*.ts)
CONFIG += lrelease embed_translations
QMAKE_LRELEASE_FLAGS += -nounfinished -silent
