qt_add_executable(meld-qt main.cpp)
target_link_libraries(meld-qt PRIVATE meldui)
set_target_properties(meld-qt PROPERTIES
    OUTPUT_NAME meld-qt
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE ON)

# The icon that Explorer, the taskbar and shortcuts show for the program, and
# its version information (tools/generate_windows_icon.py writes the icon)
if(WIN32)
    set(meld_rc_icon "${CMAKE_CURRENT_SOURCE_DIR}/meld-qt.ico")
    set(meld_rc_copyright "Copyright (c) 2026 Alexander R.")
    configure_file(meld-qt.rc.in meld-qt.rc @ONLY)
    target_sources(meld-qt PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/meld-qt.rc")
endif()

install(TARGETS meld-qt
    BUNDLE DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# windeployqt, run on the installed program, copies the libraries, plugins and
# translations it needs into the prefix, with a qt.conf that finds them there
if(MELD_DEPLOY_QT)
    qt_generate_deploy_app_script(TARGET meld-qt OUTPUT_SCRIPT meld_deploy_script)
    install(SCRIPT "${meld_deploy_script}")
endif()
