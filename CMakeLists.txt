cmake_minimum_required(VERSION 3.21)

project(MeldQt
    VERSION 1.0.0
    DESCRIPTION "Visual diff and merge tool"
    LANGUAGES CXX)

# The cooperative task scheduler is built on C++23 std::generator, which is
# the closest equivalent to the Python generators used by the original code.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MELD_SINGLE_INSTANCE "Build single-instance mode, which opens comparisons in a running Meld Qt" ON)
option(MELD_SINGLE_INSTANCE_NETWORK
       "Implement single-instance mode with local sockets from Qt Network, rather than shared memory" OFF)

set(meld_qt_components Core Gui Widgets Concurrent LinguistTools)
if(MELD_SINGLE_INSTANCE AND MELD_SINGLE_INSTANCE_NETWORK)
    list(APPEND meld_qt_components Network)
endif()
find_package(Qt6 6.8 REQUIRED COMPONENTS ${meld_qt_components})
qt_standard_project_setup()
include(GNUInstallDirs)

option(MELD_BUILD_TESTS "Build the Meld Qt test suite" ON)

# project(VERSION) has to be numeric, so this is what can name a git commit
set(MELD_VERSION "" CACHE STRING "The version that the program reports (default: the project's version)")
if(MELD_VERSION)
    set(meld_version "${MELD_VERSION}")
else()
    set(meld_version "${PROJECT_VERSION}")
endif()

# Desktop integration is for freedesktop.org desktops (Linux and BSDs)
if(UNIX AND NOT APPLE)
    option(MELD_INSTALL_DESKTOP_INTEGRATION
           "Install the desktop file, icons and the MIME type of saved comparisons with the program" ON)
else()
    set(MELD_INSTALL_DESKTOP_INTEGRATION OFF)
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

# GCC wrongly warns about every std::generator function it inlines the
# allocation of
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_compile_options(-Wno-mismatched-new-delete)
endif()

add_compile_definitions(
    QT_NO_CAST_FROM_BYTEARRAY
    QT_NO_URL_CAST_FROM_STRING
    MELD_VERSION="${meld_version}")

if(MELD_BUILD_TESTS)
    enable_testing()
endif()

add_subdirectory(src)
add_subdirectory(translations)
if(MELD_INSTALL_DESKTOP_INTEGRATION)
    add_subdirectory(data)
endif()

if(MELD_BUILD_TESTS)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    add_subdirectory(tests)
endif()
