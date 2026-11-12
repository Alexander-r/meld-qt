# Settings for all of the qmake projects, as in the top CMakeLists.txt.
#
# The options are set with qmake's CONFIG (e.g., qmake6 CONFIG+=meld_no_tests):
#  meld_no_single_instance       don't build single-instance mode
#  meld_single_instance_network  implement it with local sockets from Qt
#                                Network, rather than shared memory
#  meld_no_tests                 don't build the test suite
#  meld_no_desktop_integration   don't install the desktop file, icons and the
#                                MIME type of saved comparisons with the program
# PREFIX=<path> sets where "make install" installs to.
# MELD_VERSION=<version> sets the version that the program reports (e.g., a git
# commit), rather than the project's.

!versionAtLeast(QT_VERSION, 6.8.0): error("Meld Qt needs Qt 6.8 or newer; this qmake is for Qt $$QT_VERSION")

# As project(VERSION) in CMakeLists.txt
isEmpty(MELD_VERSION): MELD_VERSION = 1.0.0

isEmpty(PREFIX): PREFIX = /usr/local

# The cooperative task scheduler is built on C++23 std::generator, which is
# the closest equivalent to the Python generators used by the original code.
CONFIG += c++23 strict_c++ warn_on

# The libraries are found in one place, whichever configuration this is
CONFIG -= debug_and_release debug_and_release_target

gcc|clang: QMAKE_CXXFLAGS_WARN_ON += -Wno-unused-parameter

# GCC wrongly warns about every std::generator function it inlines the
# allocation of (see "Compiler warnings" in CLAUDE.md)
gcc:!clang: QMAKE_CXXFLAGS_WARN_ON += -Wno-mismatched-new-delete

DEFINES += \
    QT_NO_CAST_FROM_BYTEARRAY \
    QT_NO_URL_CAST_FROM_STRING \
    MELD_VERSION=\\\"$$MELD_VERSION\\\"

OBJECTS_DIR = .obj
MOC_DIR = .moc
RCC_DIR = .rcc
