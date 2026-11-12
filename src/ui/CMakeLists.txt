# Widgets: the comparison tabs, their custom views and the main window
qt_add_library(meldui STATIC
    accelerators.cpp accelerators.h
    actiongutter.cpp actiongutter.h
    bufferselectors.cpp bufferselectors.h
    chunkmap.cpp chunkmap.h
    dirdiff.cpp dirdiff.h
    dirdifftree.cpp dirdifftree.h
    externalhelpers.cpp externalhelpers.h
    filediff.cpp filediff_layout.cpp filediff.h
    findbar.cpp findbar.h
    icons.cpp icons.h
    inlinematcher.cpp inlinematcher.h
    linkmap.cpp linkmap.h
    meldapp.cpp meldapp.h
    melddoc.cpp melddoc.h
    meldwindow.cpp meldwindow.h
    msgarea.cpp msgarea.h
    conflictcomparison.cpp conflictcomparison.h
    newdifftab.cpp newdifftab.h
    patchcomparison.cpp patchcomparison.h
    patchdialog.cpp patchdialog.h
    pathlabel.cpp pathlabel.h
    preferences.cpp preferences.h
    recent.cpp recent.h
    settings.cpp settings.h
    sourceview.cpp sourceview.h
    statusbar.cpp statusbar.h
    style.cpp style.h
    syncpoints.cpp syncpoints.h
    trash.cpp trash.h
    vcdialogs.cpp vcdialogs.h
    vcview.cpp vcview.h
)
target_include_directories(meldui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(meldui PUBLIC meldcore Qt6::Widgets Qt6::Concurrent)

# Single-instance mode, with shared memory or Qt Network's local sockets
if(MELD_SINGLE_INSTANCE)
    target_sources(meldui PRIVATE singleinstance.cpp singleinstance.h singleinstance_p.h)
    target_compile_definitions(meldui PUBLIC MELD_SINGLE_INSTANCE)
    if(MELD_SINGLE_INSTANCE_NETWORK)
        target_sources(meldui PRIVATE singleinstance_localsocket.cpp)
        target_link_libraries(meldui PUBLIC Qt6::Network)
        target_compile_definitions(meldui PUBLIC MELD_SINGLE_INSTANCE_NETWORK)
    else()
        target_sources(meldui PRIVATE singleinstance_sharedmemory.cpp)
    endif()
endif()

# Meld's own icons (the merge action arrows and the application icon)
qt_add_resources(meldui "meld_icons"
    PREFIX "/icons"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/../resources/icons"
    FILES
        ../resources/icons/meld-change-apply-left.png
        ../resources/icons/meld-change-apply-right.png
        ../resources/icons/meld-change-copy.png
        ../resources/icons/meld-change-delete.png
        ../resources/icons/meld-change-apply-left-dark.png
        ../resources/icons/meld-change-apply-right-dark.png
        ../resources/icons/meld-change-copy-dark.png
        ../resources/icons/meld-change-delete-dark.png
        ../resources/icons/meld-qt.svg
        ../resources/icons/vc-pull-symbolic.svg
        ../resources/icons/vc-push-symbolic.svg
)
