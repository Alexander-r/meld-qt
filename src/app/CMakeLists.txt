qt_add_executable(meld-qt main.cpp)
target_link_libraries(meld-qt PRIVATE meldui)
set_target_properties(meld-qt PROPERTIES
    OUTPUT_NAME meld-qt
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE ON)

install(TARGETS meld-qt
    BUNDLE DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
