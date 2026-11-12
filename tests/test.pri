# One test, from the source file named after its project. These projects are
# all in this folder, so each has its own folders for the generated files.
TEMPLATE = app
CONFIG += testcase no_testcase_installs console
CONFIG -= app_bundle
QT += testlib

include(../meld-qt.pri)

TARGET = $$replace($$list($$basename(_PRO_FILE_)), \\.pro$, )
SOURCES += $${TARGET}.cpp
HEADERS += testutils.h
DEFINES += MELD_TEST_DATA_DIR=$$shell_quote(\"$$PWD/data\")

OBJECTS_DIR = .obj/$$TARGET
MOC_DIR = .moc/$$TARGET
RCC_DIR = .rcc/$$TARGET

# For "make check"
offscreen.name = QT_QPA_PLATFORM
offscreen.value = offscreen
QT_TOOL_ENV += offscreen
