# Core library: the diff engine and document model, independent of widgets
add_library(meldcore STATIC
    core/diffchunk.cpp core/diffchunk.h
    core/myers.cpp core/myers.h
    core/linesource.cpp core/linesource.h
    core/diffutil.cpp core/diffutil.h
    core/merge.cpp core/merge.h
    core/misc.cpp core/misc.h
    core/filters.cpp core/filters.h
    core/iohelpers.cpp core/iohelpers.h
    core/archive.cpp core/archive.h
    core/patch.cpp core/patch.h
    core/conflictmarkers.cpp core/conflictmarkers.h
    core/filecompare.cpp core/filecompare.h
    core/task.cpp core/task.h
    core/textfile.cpp core/textfile.h
    core/translations.cpp core/translations.h
    core/meldbuffer.cpp core/meldbuffer.h
    core/unifieddiff.cpp core/unifieddiff.h
    # Version control support
    core/vc/states.h
    core/vc/vc.cpp core/vc/vc.h core/vc/vcutil.h core/vc/plugins.h
    core/vc/git.cpp
    core/vc/mercurial.cpp
    core/vc/bzr.cpp
    core/vc/svn.cpp
    core/vc/darcs.cpp
    core/vc/cvs.cpp
    # Syntax highlighting (ported from GtkSourceView)
    core/syntax/contextengine.cpp core/syntax/contextengine.h
    core/syntax/highlighter.cpp core/syntax/highlighter.h
    core/syntax/language.cpp core/syntax/language.h
    core/syntax/stylescheme.cpp core/syntax/stylescheme.h
)
target_include_directories(meldcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# QPlainTextDocumentLayout (used by the text buffers) lives in QtWidgets
target_link_libraries(meldcore PUBLIC Qt6::Core Qt6::Gui Qt6::Widgets)

# The language definitions and colour schemes for syntax highlighting, from
# GtkSourceView (see resources/language-specs/README)
file(GLOB meld_language_specs CONFIGURE_DEPENDS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
    resources/language-specs/*.lang)
file(GLOB meld_style_schemes CONFIGURE_DEPENDS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
    resources/styles/*.xml)
qt_add_resources(meldcore "meld_syntax"
    PREFIX "/"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/resources"
    FILES ${meld_language_specs} ${meld_style_schemes}
)

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/ui/CMakeLists.txt)
    add_subdirectory(ui)
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/app/CMakeLists.txt)
    add_subdirectory(app)
endif()
