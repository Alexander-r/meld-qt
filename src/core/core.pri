# Using the core library, for its own project too, as PUBLIC in CMake
INCLUDEPATH += $$PWD/..

# QPlainTextDocumentLayout (used by the text buffers) lives in QtWidgets
QT += core gui widgets

!staticlib {
    MELD_CORE_DIR = $$MELD_BUILD_ROOT/src/core
    MELD_CORE_LIB = $$MELD_CORE_DIR/$${QMAKE_PREFIX_STATICLIB}meldcore.$$QMAKE_EXTENSION_STATICLIB
    # Nothing refers to the resources' object file, so the linker would leave
    # it out if it was only in the library: it's linked directly instead
    MELD_CORE_RESOURCES = \
        $$MELD_CORE_DIR/.obj/qrc_qmake_meld_syntax$$QMAKE_EXT_OBJ \
        $$MELD_CORE_DIR/.obj/qrc_qmake_qmake_qm_files$$QMAKE_EXT_OBJ
    LIBS += $$MELD_CORE_RESOURCES $$MELD_CORE_LIB
    PRE_TARGETDEPS += $$MELD_CORE_RESOURCES $$MELD_CORE_LIB
}
