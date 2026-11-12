# qmake project for Qt Creator, alongside the CMake project. They're separate
# descriptions of the same build: a source file that's added to one needs
# adding to the other.
include(meld-qt.pri)

TEMPLATE = subdirs

SUBDIRS += core ui app
core.subdir = src/core
ui.subdir = src/ui
ui.depends = core
app.subdir = src/app
app.depends = ui

# Desktop integration is for freedesktop.org desktops (Linux and BSDs)
unix:!macx:!meld_no_desktop_integration: SUBDIRS += data

!meld_no_tests {
    SUBDIRS += tests
    tests.depends = core ui
}

# "make update_translations" updates the translations from the sources' messages
# with lupdate, as CMake's target of that name does (see translations/README).
# meld-qt_en.ts only has the plural forms of English messages.
qtPrepareTool(MELD_LUPDATE, lupdate)
meld_translations = $$files($$PWD/translations/meld-qt_*.ts)
meld_translations = $$replace(meld_translations, ^.*/meld-qt_en\\.ts$, )
for (ts, meld_translations): meld_ts_args += $$shell_quote($$ts)
meld_lupdate = $$MELD_LUPDATE -I $$shell_quote($$PWD/src) $$shell_quote($$PWD/src) -locations relative -no-obsolete
update_translations.commands = \
    $$meld_lupdate -ts $$meld_ts_args && \
    $$meld_lupdate -pluralonly -ts $$shell_quote($$PWD/translations/meld-qt_en.ts)
QMAKE_EXTRA_TARGETS += update_translations

OTHER_FILES += README.md
