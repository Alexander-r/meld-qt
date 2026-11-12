# Using the widgets library, for its own project too, as PUBLIC in CMake
INCLUDEPATH += $$PWD
QT += widgets concurrent

!meld_no_single_instance {
    DEFINES += MELD_SINGLE_INSTANCE
    meld_single_instance_network {
        DEFINES += MELD_SINGLE_INSTANCE_NETWORK
        QT += network
    }
}

!staticlib {
    MELD_UI_DIR = $$MELD_BUILD_ROOT/src/ui
    MELD_UI_LIB = $$MELD_UI_DIR/$${QMAKE_PREFIX_STATICLIB}meldui.$$QMAKE_EXTENSION_STATICLIB
    # See core.pri for why the resources' object file is linked directly
    MELD_UI_RESOURCES = $$MELD_UI_DIR/.obj/qrc_qmake_meld_icons$$QMAKE_EXT_OBJ
    LIBS += $$MELD_UI_RESOURCES $$MELD_UI_LIB
    PRE_TARGETDEPS += $$MELD_UI_RESOURCES $$MELD_UI_LIB
}

# Last, for the link order
include(../core/core.pri)
