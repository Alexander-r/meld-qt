# Test suite: ports of the original's Python tests, plus differential
# tests against reference data generated from the original implementation
# (see tools/generate_reference_data.py). "make check" runs them. qmake needs
# a project for each program, which here says which library it tests.
TEMPLATE = subdirs

include(../meld-qt.pri)

SUBDIRS += \
    tst_matchers.pro \
    tst_diffutil.pro \
    tst_merge.pro \
    tst_misc.pro \
    tst_filters.pro \
    tst_iohelpers.pro \
    tst_bufferlines.pro \
    tst_chunkactions.pro \
    tst_filtertext.pro \
    tst_filecompare.pro \
    tst_task.pro \
    tst_textformat.pro \
    tst_vc.pro \
    tst_syntax.pro \
    tst_actiongutter.pro \
    tst_style.pro \
    tst_accelerators.pro \
    tst_filediff.pro \
    tst_dirdiff.pro \
    tst_window.pro \
    tst_archive.pro \
    tst_patch.pro \
    tst_conflicts.pro \
    tst_vcview.pro \
    tst_translations.pro \
    tst_vcrevisions.pro \
    tst_highlighting.pro
