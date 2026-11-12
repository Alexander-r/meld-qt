cmake_minimum_required(VERSION 3.21)
project(Sample VERSION 1.0.0 LANGUAGES CXX)

option(SAMPLE_BUILD_TESTS "Build the tests" ON)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 6.8 QUIET COMPONENTS Widgets)
if(NOT Qt6_FOUND OR Qt6_DIR STREQUAL "Qt6_DIR-NOTFOUND")
    message(FATAL_ERROR "Qt 6.8 is needed")
endif()

add_executable(sample main.cpp)
target_link_libraries(sample PRIVATE Qt6::Widgets)
set_target_properties(sample PROPERTIES WIN32_EXECUTABLE TRUE AUTOMOC YES)

if(SAMPLE_BUILD_TESTS AND NOT CMAKE_CROSSCOMPILING)
    enable_testing()
    add_test(NAME sample COMMAND sample --version)
endif()
