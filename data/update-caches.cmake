# Run at installation: update the caches of desktop files, MIME types and
# icons for the installed files, unless they're staged for a package

if(NOT "$ENV{DESTDIR}" STREQUAL "")
    return()
endif()

# meld_install_datadir is set by the install rules before this
set(datadir "${CMAKE_INSTALL_PREFIX}/${meld_install_datadir}")
foreach(update IN ITEMS
        "update-desktop-database;-q;${datadir}/applications"
        "update-mime-database;${datadir}/mime"
        "gtk-update-icon-cache;-q;-t;-f;${datadir}/icons/hicolor")
    list(POP_FRONT update program)
    # find_program() doesn't search again for a variable that's set
    unset(meld_update_program)
    find_program(meld_update_program ${program} NO_CACHE)
    if(meld_update_program)
        list(JOIN update " " arguments)
        message(STATUS "Updating: ${program} ${arguments}")
        execute_process(COMMAND ${meld_update_program} ${update} RESULT_VARIABLE result)
        if(NOT result EQUAL 0)
            message(WARNING "${program} failed; menus, file types or icons may need it run again")
        endif()
    endif()
endforeach()
