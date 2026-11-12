# The translations (see the README here), built into the core library as
# resources: :/i18n/meld-qt_<language>.qm. meld-qt_en.ts only has the plural
# forms of English messages.
#
# The update_translations target updates the .ts files from the sources with
# lupdate; release_translations builds them, which the build does anyway.
file(GLOB meld_ts_files CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/meld-qt_*.ts)
list(FILTER meld_ts_files EXCLUDE REGEX "/meld-qt_en\\.ts$")

# lupdate reads the sources that are built, and these too, so that the
# translations have the messages of every configuration
set(meld_optional_sources)
get_target_property(meld_ui_sources meldui SOURCES)
foreach(file IN ITEMS singleinstance.cpp singleinstance.h singleinstance_p.h
                      singleinstance_localsocket.cpp singleinstance_sharedmemory.cpp)
    if(NOT file IN_LIST meld_ui_sources)
        list(APPEND meld_optional_sources ${PROJECT_SOURCE_DIR}/src/ui/${file})
    endif()
endforeach()

# The icons built into the widgets library aren't sources to read
get_target_property(meld_ui_sources meldui SOURCES)
list(FILTER meld_ui_sources INCLUDE REGEX "\\.(png|svg)$")
set_property(TARGET meldui APPEND PROPERTY QT_EXCLUDE_SOURCES_FROM_TRANSLATION ${meld_ui_sources})

qt_add_translations(TARGETS meldcore
    SOURCE_TARGETS meldcore meldui meld-qt
    SOURCES ${meld_optional_sources}
    INCLUDE_DIRECTORIES ${PROJECT_SOURCE_DIR}/src
    TS_FILES ${meld_ts_files}
    PLURALS_TS_FILE ${CMAKE_CURRENT_SOURCE_DIR}/meld-qt_en.ts
    QM_FILES_OUTPUT_VARIABLE meld_qm_files
    # Only the messages the sources have: none that have gone from them
    LUPDATE_OPTIONS -locations relative -no-obsolete
    # Translations that aren't finished (e.g., made from the original's by
    # guesswork) are left out, as gettext leaves out fuzzy ones
    LRELEASE_OPTIONS -nounfinished -silent)

# By their names alone (RESOURCE_PREFIX would name them relative to the
# library's build folder)
qt_add_resources(meldcore "meld_translations"
    PREFIX "/i18n"
    BASE "${CMAKE_CURRENT_BINARY_DIR}"
    FILES ${meld_qm_files})
