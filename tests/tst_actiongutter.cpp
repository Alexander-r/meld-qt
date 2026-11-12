#include "actiongutter.h"

#include <QTest>

using namespace meld;

using OptionalMode = std::optional<ActionMode>;
Q_DECLARE_METATYPE(OptionalMode)
Q_DECLARE_METATYPE(meld::ActionMode)
Q_DECLARE_METATYPE(meld::DiffChunk)

namespace {

DiffChunk makeChunk(ChunkTag tag)
{
    return DiffChunk{tag, 0, 1, 0, 1};
}

const char *modeName(const OptionalMode &mode)
{
    if (!mode)
        return "None";
    switch (*mode) {
    case ActionMode::Replace:
        return "Replace";
    case ActionMode::Delete:
        return "Delete";
    case ActionMode::Insert:
        return "Insert";
    }
    return "?";
}

} // namespace

class TestActionGutter : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void classifyChangeActions_data()
    {
        QTest::addColumn<ActionMode>("mode");
        QTest::addColumn<bool>("sourceEditable");
        QTest::addColumn<bool>("targetEditable");
        QTest::addColumn<DiffChunk>("chunk");
        QTest::addColumn<OptionalMode>("expectedAction");

        const auto add = [](ActionMode mode, bool source, bool target, ChunkTag tag, OptionalMode expected) {
            const QByteArray name = QByteArray(modeName(mode)) + " mode, editable (" + (source ? "T" : "F") + ", "
                + (target ? "T" : "F") + "), " + chunkTagName(tag).toLatin1() + " chunk";
            QTest::newRow(name.constData()) << mode << source << target << makeChunk(tag) << expected;
        };
        using enum ActionMode;
        constexpr OptionalMode None = std::nullopt;

        // Replace mode with replace chunks
        add(Replace, true, true, ChunkTag::Replace, Replace);
        add(Replace, true, false, ChunkTag::Replace, Delete);
        add(Replace, false, true, ChunkTag::Replace, Replace);
        add(Replace, false, false, ChunkTag::Replace, None);
        // Replace mode with delete chunks
        add(Replace, true, true, ChunkTag::Delete, Replace);
        add(Replace, true, false, ChunkTag::Delete, Delete);
        add(Replace, false, true, ChunkTag::Delete, Replace);
        add(Replace, false, false, ChunkTag::Delete, None);
        // Delete mode makes a slightly weird choice to remove non-delete
        // actions while in delete mode; insert mode makes the opposite
        // choice
        //
        // Delete mode with replace chunks
        add(Delete, true, true, ChunkTag::Replace, Delete);
        add(Delete, true, false, ChunkTag::Replace, Delete);
        add(Delete, false, true, ChunkTag::Replace, None);
        add(Delete, false, false, ChunkTag::Replace, None);
        // Delete mode with delete chunks
        add(Delete, true, true, ChunkTag::Delete, Delete);
        add(Delete, true, false, ChunkTag::Delete, Delete);
        add(Delete, false, true, ChunkTag::Delete, None);
        add(Delete, false, false, ChunkTag::Delete, None);
        // Insert mode with replace chunks
        add(Insert, true, true, ChunkTag::Replace, Insert);
        add(Insert, true, false, ChunkTag::Replace, Delete);
        add(Insert, false, true, ChunkTag::Replace, Insert);
        add(Insert, false, false, ChunkTag::Replace, None);
        // Insert mode with delete chunks
        add(Insert, true, true, ChunkTag::Delete, Replace);
        add(Insert, true, false, ChunkTag::Delete, Delete);
        add(Insert, false, true, ChunkTag::Delete, Replace);
        add(Insert, false, false, ChunkTag::Delete, None);
        // Insert chunks have no text on this side: all that can be done is
        // to remove the target's text, by making it match. This isn't in the
        // original, where they have no actions.
        add(Replace, true, true, ChunkTag::Insert, Replace);
        add(Replace, true, false, ChunkTag::Insert, None);
        add(Replace, false, true, ChunkTag::Insert, Replace);
        add(Replace, false, false, ChunkTag::Insert, None);
        // Delete mode with insert chunks
        add(Delete, true, true, ChunkTag::Insert, None);
        add(Delete, true, false, ChunkTag::Insert, None);
        add(Delete, false, true, ChunkTag::Insert, None);
        add(Delete, false, false, ChunkTag::Insert, None);
        // Insert mode with insert chunks
        add(Insert, true, true, ChunkTag::Insert, None);
        add(Insert, true, false, ChunkTag::Insert, None);
        add(Insert, false, true, ChunkTag::Insert, None);
        add(Insert, false, false, ChunkTag::Insert, None);
    }

    void classifyChangeActions()
    {
        QFETCH(ActionMode, mode);
        QFETCH(bool, sourceEditable);
        QFETCH(bool, targetEditable);
        QFETCH(DiffChunk, chunk);
        QFETCH(OptionalMode, expectedAction);

        const OptionalMode action = ActionGutter::classifyChangeActions(chunk, mode, sourceEditable, targetEditable);
        QCOMPARE(modeName(action), modeName(expectedAction));
    }

    // Conflicts are classified by their shape, as a normal two-way change
    void classifyConflictActions_data()
    {
        QTest::addColumn<DiffChunk>("chunk");
        QTest::addColumn<OptionalMode>("expectedAction");

        QTest::newRow("insert-shaped conflict") << DiffChunk{ChunkTag::Conflict, 2, 2, 2, 4} << OptionalMode();
        QTest::newRow("delete-shaped conflict")
            << DiffChunk{ChunkTag::Conflict, 2, 4, 2, 2} << OptionalMode(ActionMode::Replace);
        QTest::newRow("replace-shaped conflict")
            << DiffChunk{ChunkTag::Conflict, 2, 4, 2, 5} << OptionalMode(ActionMode::Insert);
    }

    void classifyConflictActions()
    {
        QFETCH(DiffChunk, chunk);
        QFETCH(OptionalMode, expectedAction);

        const OptionalMode action = ActionGutter::classifyChangeActions(chunk, ActionMode::Insert, true, true);
        QCOMPARE(modeName(action), modeName(expectedAction));
    }

    void classifyInsertShapedConflictInReplaceMode()
    {
        const DiffChunk chunk{ChunkTag::Conflict, 2, 2, 2, 4};
        QCOMPARE(modeName(ActionGutter::classifyChangeActions(chunk, ActionMode::Replace, true, true)), "Replace");
        QCOMPARE(modeName(ActionGutter::classifyChangeActions(chunk, ActionMode::Replace, true, false)), "None");
    }
};

QTEST_MAIN(TestActionGutter)
#include "tst_actiongutter.moc"
