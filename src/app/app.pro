TEMPLATE = app
TARGET = meld-qt
CONFIG += windows

include(../../meld-qt.pri)
include(../ui/ui.pri)

SOURCES += main.cpp

macx: target.path = $$PREFIX
else: target.path = $$PREFIX/bin
INSTALLS += target
