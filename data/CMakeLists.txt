# Desktop integration (freedesktop.org): the application's desktop file, so
# that it's in application menus and can open files, its icons, and the MIME
# type of saved comparisons (*.meldcmp).

get_target_property(meld_executable meld-qt OUTPUT_NAME)
set(MELD_EXECUTABLE ${meld_executable})
configure_file(Meld-Qt.desktop.in Meld-Qt.desktop @ONLY)
set(meld_desktop_file ${CMAKE_CURRENT_BINARY_DIR}/Meld-Qt.desktop)

install(FILES ${meld_desktop_file} DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES Meld-Qt.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/mime/packages)
install(FILES icons/hicolor/scalable/apps/Meld-Qt.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
install(FILES icons/hicolor/symbolic/apps/Meld-Qt-symbolic.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/symbolic/apps)

# As GNOME's post-install step: the desktop, MIME and icon caches are
# updated, if their programs are installed, unless the files are being
# staged for a package (DESTDIR), whose installation updates them
install(CODE "set(meld_install_datadir \"${CMAKE_INSTALL_DATADIR}\")")
install(SCRIPT update-caches.cmake)

if(MELD_BUILD_TESTS)
    find_program(DESKTOP_FILE_VALIDATE desktop-file-validate)
    if(DESKTOP_FILE_VALIDATE)
        add_test(NAME validate_desktop_file COMMAND ${DESKTOP_FILE_VALIDATE} ${meld_desktop_file})
    endif()
endif()
