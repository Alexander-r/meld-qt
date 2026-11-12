// Integration tests for file comparisons: loading, chunk actions, editing,
// undo, saving and merging, driven through the same window and actions as
// the user interface.

#include "actiongutter.h"
#include "core/diffutil.h"
#include "core/meldbuffer.h"
#include "filediff.h"
#include "linkmap.h"
#include "meldwindow.h"
#include "settings.h"
#include "sourceview.h"

#include <QFile>
#include <QPointer>
#include <QScopeGuard>
#include <QScrollBar>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBlock>

using namespace meld;

namespace {

QStringList chunkStrings(const std::vector<DiffChunk> &chunks)
{
    QStringList result;
    for (const DiffChunk &c : chunks) {
        result.append(QStringLiteral("%1 %2 %3 %4 %5")
                          .arg(chunkTagName(c.tag))
                          .arg(c.start_a)
                          .arg(c.end_a)
                          .arg(c.start_b)
                          .arg(c.end_b));
    }
    return result;
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return QByteArray();
    return file.readAll();
}

} // namespace

class TestFileDiff : public QObject {
    Q_OBJECT

private:
    QTemporaryDir dir_;
    std::unique_ptr<MeldWindow> window_;
    int fileCounter_ = 0;

    QString writeFile(const QByteArray &content, const QString &suffix = QStringLiteral(".txt"))
    {
        const QString path = dir_.filePath(QStringLiteral("file%1%2").arg(++fileCounter_).arg(suffix));
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly))
            qFatal("Couldn't write test file");
        file.write(content);
        return path;
    }

    void settle()
    {
        for (int i = 0; i < 100; ++i) {
            window_->runPendingTasks();
            QCoreApplication::processEvents();
            if (!window_->scheduler().tasksPending())
                break;
        }
    }

    FileDiff *open(const QStringList &files)
    {
        FileDiff *doc = window_->appendFilediff(files);
        window_->tabWidget()->setCurrentWidget(doc);
        settle();
        if (!doc->comparisonReady())
            qWarning("Comparison wasn't ready");
        return doc;
    }

    // Clicks the button of a gutter's chunk, as painted
    void clickGutterButton(FileDiff *doc, int fromPane, int toPane, const DiffChunk &chunk)
    {
        ActionGutter *gutter = nullptr;
        for (ActionGutter *candidate : doc->findChildren<ActionGutter *>()) {
            if (candidate->sourceView() == doc->textView(fromPane) && candidate->targetView() == doc->textView(toPane))
                gutter = candidate;
        }
        QVERIFY(gutter);
        QVERIFY(gutter->chunks() == std::vector<DiffChunk>{chunk});
        (void)gutter->grab(); // the buttons are found as they're painted

        SourceView *source = gutter->sourceView();
        const int translate = source->viewport()->mapTo(gutter->window(), QPoint(0, 0)).y()
            - gutter->mapTo(gutter->window(), QPoint(0, 0)).y();
        const bool afterLastLine = chunk.start_a >= source->document()->blockCount();
        const int offset = afterLastLine ? -int(source->lineHeight() / 2) : int(source->lineHeight() / 2);
        const QPoint point(gutter->width() / 2, int(source->yForLine(chunk.start_a)) + translate + offset);
        QTest::mouseClick(gutter, Qt::LeftButton, {}, point);
        settle();
    }

    void trigger(FileDiff *doc, const char *name)
    {
        QAction *action = doc->action(QLatin1String(name));
        QVERIFY2(action, name);
        QVERIFY2(action->isEnabled(), name);
        action->trigger();
        settle();
    }

    static QStringList chunks(const FileDiff *doc, int from = 0, int to = 1)
    {
        return chunkStrings(doc->differ()->pairChanges(from, to));
    }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(dir_.isValid());
        MeldSettings::useSettingsFile(dir_.filePath(QStringLiteral("settings.ini")));
    }

    void init()
    {
        window_ = std::make_unique<MeldWindow>();
        window_->show();
        QVERIFY(QTest::qWaitForWindowExposed(window_.get()));
        window_->activateWindow();
    }

    void cleanup()
    {
        // Discard any unsaved changes so that closing doesn't prompt
        for (MeldDoc *doc : window_->docs()) {
            if (auto *filediff = qobject_cast<FileDiff *>(doc)) {
                for (int pane = 0; pane < filediff->numPanes(); ++pane)
                    filediff->textBuffer(pane)->setModified(false);
            }
        }
        window_.reset();
    }

    void currentLineIsHighlightedInTheFocusedPane()
    {
        MeldSettings::instance()->setValue(QStringLiteral("highlight-current-line"), true);
        const QString left = writeFile("one\ntwo\nthree\n");
        const QString right = writeFile("one\ntwo\nthree\n");
        FileDiff *doc = open({left, right});
        QVERIFY(QTest::qWaitForWindowActive(window_.get()));
        SourceView *views[] = {doc->textView(0), doc->textView(1)};
        for (SourceView *view : views) {
            QVERIFY(view->highlightCurrentLine());
            view->placeCursorAtLine(1);
        }
        // A pixel of the second line, right of its text
        const auto lineColour = [](SourceView *view) {
            const SourceView::VisibleLine line = view->visibleLines().at(1);
            view->viewport()->repaint();
            return view->viewport()->grab().toImage().pixelColor(view->viewport()->width() - 4,
                                                                 int(line.top + line.height / 2));
        };

        views[0]->setFocus();
        QTRY_VERIFY(views[0]->hasFocus());
        const QColor highlighted = lineColour(views[0]);
        const QColor plain = lineColour(views[1]);
        QVERIFY(highlighted != plain);

        views[1]->setFocus();
        QTRY_VERIFY(views[1]->hasFocus());
        QCOMPARE(lineColour(views[0]), plain);
        QCOMPARE(lineColour(views[1]), highlighted);
        MeldSettings::instance()->resetValue(QStringLiteral("highlight-current-line"));
    }

    void twoWayComparison()
    {
        const QString left = writeFile("a\nb\nc\nd\ne\n");
        const QString right = writeFile("a\nB\nc\ne\nf\n");
        FileDiff *doc = open({left, right});

        QCOMPARE(doc->numPanes(), 2);
        QCOMPARE(doc->textBuffer(0)->text(), QStringLiteral("a\nb\nc\nd\ne\n"));
        QCOMPARE(doc->textBuffer(1)->text(), QStringLiteral("a\nB\nc\ne\nf\n"));
        QCOMPARE(chunks(doc), (QStringList{"replace 1 2 1 2", "delete 3 4 3 3", "insert 5 5 4 5"}));
        QCOMPARE(doc->tabTitle(), QFileInfo(left).fileName() + QStringLiteral(" — ") + QFileInfo(right).fileName());

        const std::optional<Comparison> comparison = doc->comparison();
        QVERIFY(comparison);
        QCOMPARE(comparison->type, RecentType::File);
        QCOMPARE(comparison->paths, (QStringList{left, right}));
    }

    void identicalFilesShowAMessage()
    {
        const QString left = writeFile("same\n");
        const QString right = writeFile("same\n");
        FileDiff *doc = open({left, right});
        QVERIFY(chunks(doc).isEmpty());
        QVERIFY(doc->differ()->sequencesIdentical());
    }

    void navigateChanges()
    {
        FileDiff *doc = open({writeFile("a\nb\nc\nd\ne\n"), writeFile("a\nB\nc\ne\nf\n")});
        doc->moveCursor(0, 0);
        settle();
        QVERIFY(!doc->action(QStringLiteral("previous-change"))->isEnabled());

        trigger(doc, "next-change");
        QCOMPARE(doc->textView(0)->textCursor().blockNumber(), 1);
        QCOMPARE(doc->currentChunk(), std::optional<int>(0));
        trigger(doc, "next-change");
        QCOMPARE(doc->textView(0)->textCursor().blockNumber(), 3);
        trigger(doc, "previous-change");
        QCOMPARE(doc->textView(0)->textCursor().blockNumber(), 1);
    }

    void pushChangeUndoRedo()
    {
        FileDiff *doc = open({writeFile("a\nb\nc\nd\ne\n"), writeFile("a\nB\nc\ne\nf\n")});
        doc->moveCursor(0, 1);
        settle();
        QCOMPARE(doc->currentChunk(), std::optional<int>(0));

        trigger(doc, "file-push-right");
        QCOMPARE(doc->textBuffer(1)->text(), QStringLiteral("a\nb\nc\ne\nf\n"));
        QVERIFY(doc->textBuffer(1)->isModified());
        QVERIFY(!doc->textBuffer(0)->isModified());
        QCOMPARE(chunks(doc), (QStringList{"delete 3 4 3 3", "insert 5 5 4 5"}));
        QVERIFY(doc->tabTitle().contains(QLatin1Char('*')));

        trigger(doc, "undo");
        QCOMPARE(doc->textBuffer(1)->text(), QStringLiteral("a\nB\nc\ne\nf\n"));
        QVERIFY(!doc->textBuffer(1)->isModified());
        QCOMPARE(chunks(doc), (QStringList{"replace 1 2 1 2", "delete 3 4 3 3", "insert 5 5 4 5"}));

        trigger(doc, "redo");
        QCOMPARE(doc->textBuffer(1)->text(), QStringLiteral("a\nb\nc\ne\nf\n"));
        QCOMPARE(chunks(doc), (QStringList{"delete 3 4 3 3", "insert 5 5 4 5"}));
    }

    void pullAndDeleteChanges()
    {
        FileDiff *doc = open({writeFile("a\nb\nc\nd\ne\n"), writeFile("a\nB\nc\ne\nf\n")});

        // Pulling a deletion isn't possible; that's what delete is for
        doc->moveCursor(0, 3);
        settle();
        QVERIFY(!doc->action(QStringLiteral("file-pull-right"))->isEnabled());

        // Pull the change of "b" from the right pane into the left
        doc->moveCursor(0, 1);
        settle();
        trigger(doc, "file-pull-right");
        QCOMPARE(doc->textBuffer(0)->text(), QStringLiteral("a\nB\nc\nd\ne\n"));

        // Delete the inserted "f" from the right pane
        doc->moveCursor(1, 4);
        settle();
        trigger(doc, "file-delete");
        QCOMPARE(doc->textBuffer(1)->text(), QStringLiteral("a\nB\nc\ne\n"));
        QCOMPARE(chunks(doc), (QStringList{"delete 3 4 3 3"}));

        // Undo works across panes in reverse order of the edits
        trigger(doc, "undo");
        QCOMPARE(doc->textBuffer(1)->text(), QStringLiteral("a\nB\nc\ne\nf\n"));
        QCOMPARE(doc->textBuffer(0)->text(), QStringLiteral("a\nB\nc\nd\ne\n"));
        trigger(doc, "undo");
        QCOMPARE(doc->textBuffer(0)->text(), QStringLiteral("a\nb\nc\nd\ne\n"));
        QVERIFY(!doc->action(QStringLiteral("undo"))->isEnabled());
    }

    void gutterRemovesAFragmentFromTheEmptySide()
    {
        window_->resize(800, 600);
        window_->show();
        FileDiff *doc = open({writeFile("a\nc\n"), writeFile("a\nb1\nb2\nc\n")});
        QCOMPARE(chunks(doc), (QStringList{"insert 1 1 1 3"}));
        const DiffChunk chunk{ChunkTag::Insert, 1, 1, 1, 3};

        // The button on the side without the text removes it from the other
        clickGutterButton(doc, 0, 1, chunk);
        QCOMPARE(doc->textBuffer(1)->text(), QStringLiteral("a\nc\n"));
        QCOMPARE(doc->textBuffer(0)->text(), QStringLiteral("a\nc\n"));
        QVERIFY(chunks(doc).isEmpty());

        trigger(doc, "undo");
        QCOMPARE(doc->textBuffer(1)->text(), QStringLiteral("a\nb1\nb2\nc\n"));
        QVERIFY(!doc->action(QStringLiteral("undo"))->isEnabled());

        // The button on the side with the text still copies it across
        clickGutterButton(doc, 1, 0, chunk.reversed());
        QCOMPARE(doc->textBuffer(0)->text(), QStringLiteral("a\nb1\nb2\nc\n"));
        QCOMPARE(doc->textBuffer(1)->text(), QStringLiteral("a\nb1\nb2\nc\n"));
    }

    void gutterRemovesAFragmentFromTheLeftPane()
    {
        window_->resize(800, 600);
        window_->show();
        FileDiff *doc = open({writeFile("a\nb\nc\n"), writeFile("a\nc\n")});
        QCOMPARE(chunks(doc), (QStringList{"delete 1 2 1 1"}));

        clickGutterButton(doc, 1, 0, DiffChunk{ChunkTag::Insert, 1, 1, 1, 2});
        QCOMPARE(doc->textBuffer(0)->text(), QStringLiteral("a\nc\n"));
    }

    // Without a final line break, the fragment follows the other file's last
    // line, and removing it has to remove the line break before it too
    void gutterRemovesAFragmentAfterTheLastLine()
    {
        window_->resize(800, 600);
        window_->show();
        FileDiff *doc = open({writeFile("a"), writeFile("a\nb")});
        const DiffChunk chunk{ChunkTag::Insert, 1, 1, 1, 2};
        QCOMPARE(chunks(doc), (QStringList{"insert 1 1 1 2"}));

        clickGutterButton(doc, 0, 1, chunk);
        QCOMPARE(doc->textBuffer(1)->text(), QStringLiteral("a"));
        QVERIFY(chunks(doc).isEmpty());
    }

    void gutterRemovesAFragmentFromTheMiddlePane()
    {
        window_->resize(800, 600);
        window_->show();
        FileDiff *doc = open({writeFile("a\nc\n"), writeFile("a\nb\nc\n"), writeFile("a\nb\nc\n")});
        clickGutterButton(doc, 0, 1, DiffChunk{ChunkTag::Insert, 1, 1, 1, 2});
        QCOMPARE(doc->textBuffer(1)->text(), QStringLiteral("a\nc\n"));
        QCOMPARE(doc->textBuffer(2)->text(), QStringLiteral("a\nb\nc\n"));
    }

    void gutterBandEnclosesTheButton_data()
    {
        QTest::addColumn<QByteArray>("left");
        QTest::addColumn<QByteArray>("right");
        QTest::addColumn<int>("emptyPane");
        QTest::addColumn<int>("line");

        QTest::newRow("empty on the left") << QByteArray("a\nc\n") << QByteArray("a\nb1\nb2\nc\n") << 0 << 1;
        QTest::newRow("empty on the right") << QByteArray("a\nb1\nb2\nc\n") << QByteArray("a\nc\n") << 1 << 1;
        QTest::newRow("after the last line") << QByteArray("a\nb") << QByteArray("a\nb\nc") << 0 << 2;
    }

    void gutterBandEnclosesTheButton()
    {
        QFETCH(QByteArray, left);
        QFETCH(QByteArray, right);
        QFETCH(int, emptyPane);
        QFETCH(int, line);

        window_->resize(800, 600);
        window_->show();
        FileDiff *doc = open({writeFile(left), writeFile(right)});
        SourceView *view = doc->textView(emptyPane);

        const auto [buttonTop, buttonHeight] = ActionGutter::buttonExtent(view, line);
        const auto [top, bottom] = ActionGutter::emptyChunkBand(view, line);
        QVERIFY(buttonHeight > view->lineHeight() / 2);
        QVERIFY(top < buttonTop);
        QVERIFY(bottom > buttonTop + buttonHeight);

        ActionGutter *gutter = nullptr;
        for (ActionGutter *candidate : doc->findChildren<ActionGutter *>()) {
            if (candidate->sourceView() == view && candidate->targetView() == doc->textView(1 - emptyPane))
                gutter = candidate;
        }
        QVERIFY(gutter);
        LinkMap *linkmap = doc->findChildren<LinkMap *>().value(0);
        QVERIFY(linkmap);

        const int y = int(buttonTop + buttonHeight / 2)
            + view->viewport()->mapTo(window_.get(), QPoint(0, 0)).y() - gutter->mapTo(window_.get(), QPoint(0, 0)).y();
        const QImage gutterImage = gutter->grab().toImage();
        const QImage linkmapImage = linkmap->grab().toImage();
        const QColor background = gutter->palette().color(QPalette::Window);

        // The band's narrow at the view, and full height at the link map,
        // which carries on from it
        const int viewX = emptyPane == 0 ? 0 : gutterImage.width() - 1;
        const int mapX = emptyPane == 0 ? gutterImage.width() - 1 : 0;
        const int linkmapX = emptyPane == 0 ? 2 : linkmapImage.width() - 3;
        QCOMPARE(gutterImage.pixelColor(viewX, y), background);
        QVERIFY(gutterImage.pixelColor(mapX, y) != background);
        QCOMPARE(linkmapImage.pixelColor(linkmapX, y), gutterImage.pixelColor(mapX, y));
    }

    // The green line runs through the line numbers, as for other changes
    void lineNumbersShowAChangeAfterTheLastLine()
    {
        MeldSettings::instance()->setValue(QStringLiteral("show-line-numbers"), true);
        const auto reset = qScopeGuard([]() {
            MeldSettings::instance()->resetValue(QStringLiteral("show-line-numbers"));
        });
        window_->resize(800, 600);
        window_->show();

        // The colour of the line numbers' column at the height of the change,
        // and below it
        const auto colours = [this](const QByteArray &left, const QByteArray &right, int pane) {
            FileDiff *doc = open({writeFile(left), writeFile(right)});
            SourceView *view = doc->textView(pane);
            auto *numbers = view->findChild<LineNumberGutter *>();
            const int y = int(view->yForLine(view->document()->blockCount()));
            const QImage image = numbers ? numbers->grab().toImage() : QImage();
            return std::pair{image.pixelColor(1, y), image.pixelColor(1, y + 6)};
        };

        const auto [leftChange, belowLeftChange] = colours("a\nb", "a\nb\nc", 0);
        QVERIFY(leftChange.isValid());
        QVERIFY(leftChange != belowLeftChange);
        const auto [rightChange, belowRightChange] = colours("a\nb\nc", "a\nb", 1);
        QVERIFY(rightChange.isValid());
        QVERIFY(rightChange != belowRightChange);
        const auto [noChange, belowNoChange] = colours("a\nb", "a\nb", 0);
        QCOMPARE(noChange, belowNoChange);
    }

    // The button of a change after the last line is on the last line, so it's
    // there whenever that part of the line is, however it's scrolled
    void gutterShowsTheButtonOnTheBottomLine_data()
    {
        QTest::addColumn<int>("emptyPane");
        QTest::newRow("empty on the left") << 0;
        QTest::newRow("empty on the right") << 1;
    }

    void gutterShowsTheButtonOnTheBottomLine()
    {
        QFETCH(int, emptyPane);

        QByteArray lines;
        for (int i = 0; i < 60; ++i)
            lines += "line " + QByteArray::number(i) + "\n";
        const QByteArray shorter = lines + "last";
        const QByteArray longer = lines + "last\nnew";

        window_->resize(800, 330);
        window_->show();
        FileDiff *doc = emptyPane == 0 ? open({writeFile(shorter), writeFile(longer)})
                                       : open({writeFile(longer), writeFile(shorter)});
        SourceView *view = doc->textView(emptyPane);
        ActionGutter *gutter = nullptr;
        for (ActionGutter *candidate : doc->findChildren<ActionGutter *>()) {
            if (candidate->sourceView() == view && candidate->targetView() == doc->textView(1 - emptyPane))
                gutter = candidate;
        }
        QVERIFY(gutter);
        LinkMap *linkmap = doc->findChildren<LinkMap *>().value(0);
        QVERIFY(linkmap);

        const QColor background = gutter->palette().color(QPalette::Window);
        const int lastLine = view->document()->blockCount();
        QScrollBar *scrollBar = view->verticalScrollBar();
        int bottomLinePosition = -1;
        for (int value = scrollBar->maximum(); value >= 0; --value) {
            scrollBar->setValue(value);
            settle();
            const auto [buttonTop, buttonHeight] = ActionGutter::buttonExtent(view, lastLine);
            const int y = int(buttonTop + buttonHeight / 2)
                + view->viewport()->mapTo(window_.get(), QPoint(0, 0)).y()
                - gutter->mapTo(window_.get(), QPoint(0, 0)).y();
            if (y >= gutter->height())
                break;

            // The band is in the gutter, and the link map carries on from it
            const QImage gutterImage = gutter->grab().toImage();
            const QImage linkmapImage = linkmap->grab().toImage();
            const QColor band = gutterImage.pixelColor(emptyPane == 0 ? gutterImage.width() - 1 : 0, y);
            QVERIFY2(band != background, qPrintable(QStringLiteral("scrolled to %1").arg(value)));
            QCOMPARE(linkmapImage.pixelColor(emptyPane == 0 ? 2 : linkmapImage.width() - 3, y), band);

            // The line that the chunk is at is below the view
            if (view->yForLine(lastLine) >= gutter->height())
                bottomLinePosition = value;
        }
        QVERIFY(bottomLinePosition >= 0);

        // ... and it works
        scrollBar->setValue(bottomLinePosition);
        settle();
        clickGutterButton(doc, emptyPane, 1 - emptyPane, DiffChunk{ChunkTag::Insert, lastLine, lastLine, lastLine, lastLine + 1});
        QCOMPARE(doc->textBuffer(1 - emptyPane)->text(), QString::fromLatin1(shorter));
    }

    // A large change earlier in the file can put the text of a change far from
    // its place in the other pane; the link map still ends its band neatly
    void gutterBandEndsNeatlyWhenItsTextIsOffScreen_data()
    {
        QTest::addColumn<int>("emptyPane");
        QTest::newRow("empty on the left") << 0;
        QTest::newRow("empty on the right") << 1;
    }

    void gutterBandEndsNeatlyWhenItsTextIsOffScreen()
    {
        QFETCH(int, emptyPane);

        QByteArray before;
        QByteArray after;
        QByteArray large;
        for (int i = 0; i < 14; ++i)
            before += "before " + QByteArray::number(i) + "\n";
        for (int i = 0; i < 7; ++i)
            after += "after " + QByteArray::number(i) + "\n";
        for (int i = 0; i < 80; ++i)
            large += "large " + QByteArray::number(i) + "\n";
        const QByteArray shorter = before + after + "last";
        const QByteArray longer = before + large + after + "last\nnew";

        window_->resize(800, 330);
        window_->show();
        FileDiff *doc = emptyPane == 0 ? open({writeFile(shorter), writeFile(longer)})
                                       : open({writeFile(longer), writeFile(shorter)});
        SourceView *view = doc->textView(emptyPane);
        SourceView *other = doc->textView(1 - emptyPane);
        ActionGutter *gutter = nullptr;
        for (ActionGutter *candidate : doc->findChildren<ActionGutter *>()) {
            if (candidate->sourceView() == view && candidate->targetView() == other)
                gutter = candidate;
        }
        QVERIFY(gutter);
        LinkMap *linkmap = doc->findChildren<LinkMap *>().value(0);
        QVERIFY(linkmap);

        const QColor background = gutter->palette().color(QPalette::Window);
        const int lastLine = view->document()->blockCount();
        const int textLine = other->document()->blockCount() - 1;
        QScrollBar *scrollBar = view->verticalScrollBar();
        bool reached = false;
        for (int value = 0; value <= scrollBar->maximum(); ++value) {
            scrollBar->setValue(value);
            settle();
            const auto [buttonTop, buttonHeight] = ActionGutter::buttonExtent(view, lastLine);
            const int y = int(buttonTop + buttonHeight / 2)
                + view->viewport()->mapTo(window_.get(), QPoint(0, 0)).y()
                - gutter->mapTo(window_.get(), QPoint(0, 0)).y();
            if (y < 0 || y >= gutter->height() || other->yForLine(textLine) <= linkmap->height())
                continue;

            reached = true;
            const QImage gutterImage = gutter->grab().toImage();
            const QImage linkmapImage = linkmap->grab().toImage();
            const QColor band = gutterImage.pixelColor(emptyPane == 0 ? gutterImage.width() - 1 : 0, y);
            QVERIFY2(band != background, qPrintable(QStringLiteral("scrolled to %1").arg(value)));
            QCOMPARE(linkmapImage.pixelColor(emptyPane == 0 ? 1 : linkmapImage.width() - 2, y), band);
        }
        QVERIFY(reached);
    }

    void gutterHasNoButtonForAReadOnlyFragment()
    {
        window_->resize(800, 600);
        window_->show();
        FileDiff *doc = open({writeFile("a\nc\n"), writeFile("a\nb\nc\n")});
        doc->textView(1)->setEditable(false);
        settle();
        clickGutterButton(doc, 0, 1, DiffChunk{ChunkTag::Insert, 1, 1, 1, 2});
        QCOMPARE(doc->textBuffer(1)->text(), QStringLiteral("a\nb\nc\n"));
    }

    void copyChangeAbove()
    {
        FileDiff *doc = open({writeFile("a\nb\nc\n"), writeFile("a\nB\nc\n")});
        doc->moveCursor(0, 1);
        settle();
        trigger(doc, "file-copy-right-up");
        QCOMPARE(doc->textBuffer(1)->text(), QStringLiteral("a\nb\nB\nc\n"));
    }

    void editingUpdatesTheComparison()
    {
        FileDiff *doc = open({writeFile("a\nb\nc\nd\ne\n"), writeFile("a\nB\nc\ne\nf\n")});
        MeldBuffer *right = doc->textBuffer(1);

        right->insertText(right->positionAtLine(3), QStringLiteral("d\n"));
        settle();
        QCOMPARE(chunks(doc), (QStringList{"replace 1 2 1 2", "insert 5 5 5 6"}));

        // Deleting across a chunk boundary
        right->deleteRange(right->positionAtLine(4), right->positionAtLine(6));
        settle();
        QCOMPARE(right->text(), QStringLiteral("a\nB\nc\nd\n"));
        QCOMPARE(chunks(doc), (QStringList{"replace 1 2 1 2", "delete 4 5 4 4"}));

        // Making the panes identical
        right->deleteRange(right->positionAtLine(1), right->positionAtLine(2));
        right->insertText(right->positionAtLine(1), QStringLiteral("b\n"));
        right->insertText(right->positionAtLine(4), QStringLiteral("e\n"));
        settle();
        QCOMPARE(right->text(), doc->textBuffer(0)->text());
        QVERIFY(chunks(doc).isEmpty());
    }

    void typingIntoAView()
    {
        FileDiff *doc = open({writeFile("abc\n"), writeFile("abc\n")});
        SourceView *view = doc->textView(1);
        doc->moveCursor(1, 0);
        QTest::keyClicks(view, QStringLiteral("x"));
        settle();
        QCOMPARE(doc->textBuffer(1)->text(), QStringLiteral("xabc\n"));
        QCOMPARE(chunks(doc), (QStringList{"replace 0 1 0 1"}));
        QTest::keyClick(view, Qt::Key_Z, Qt::ControlModifier);
        settle();
        QCOMPARE(doc->textBuffer(1)->text(), QStringLiteral("abc\n"));
        QVERIFY(chunks(doc).isEmpty());
    }

    void saveFile()
    {
        const QString left = writeFile("a\nb\n");
        const QString right = writeFile("a\nc\n");
        FileDiff *doc = open({left, right});
        doc->moveCursor(0, 1);
        settle();
        trigger(doc, "file-push-right");
        QVERIFY(doc->textBuffer(1)->isModified());

        QSignalSpy changed(doc, &MeldDoc::fileChanged);
        QVERIFY(doc->saveFile(1));
        QCOMPARE(readFile(right), QByteArray("a\nb\n"));
        QVERIFY(!doc->textBuffer(1)->isModified());
        QCOMPARE(changed.count(), 1);
        QVERIFY(!doc->tabTitle().contains(QLatin1Char('*')));
    }

    void saveKeepsNewlinesAndEncoding()
    {
        const QString left = writeFile("caf\xe9\r\nb\r\n");
        const QString right = writeFile("caf\xe9\r\nc\r\n");
        FileDiff *doc = open({left, right});
        QCOMPARE(doc->textBuffer(0)->text(), QStringLiteral("café\nb\n"));
        QVERIFY(doc->textBuffer(1)->data.format.encoding != "UTF-8");
        QCOMPARE(doc->textBuffer(1)->data.format.newline, NewlineType::CRLF);

        doc->moveCursor(0, 1);
        settle();
        trigger(doc, "file-push-right");
        QVERIFY(doc->saveFile(1));
        QCOMPARE(readFile(right), QByteArray("caf\xe9\r\nb\r\n"));
    }

    void saveAll()
    {
        const QString left = writeFile("a\n");
        const QString right = writeFile("b\n");
        FileDiff *doc = open({left, right});
        doc->textBuffer(0)->insertText(0, QStringLiteral("x"));
        doc->textBuffer(1)->insertText(0, QStringLiteral("y"));
        settle();
        trigger(doc, "save-all");
        QCOMPARE(readFile(left), QByteArray("xa\n"));
        QCOMPARE(readFile(right), QByteArray("yb\n"));
    }

    void blankPane()
    {
        FileDiff *doc = open({writeFile("a\nb\n"), QString()});
        QCOMPARE(doc->numPanes(), 2);
        QVERIFY(doc->textBuffer(1)->text().isEmpty());
        QCOMPARE(chunks(doc), (QStringList{"delete 0 2 0 0"}));

        // Comparisons with blank panes aren't recorded as recent
        QVERIFY(!doc->comparison() || doc->comparison()->paths.contains(QString()));
    }

    void threeWayConflicts()
    {
        FileDiff *doc = open({writeFile("a\nX\nc\nd\n"), writeFile("a\nb\nc\nd\n"), writeFile("a\nY\nc\nD\n")});
        QCOMPARE(doc->numPanes(), 3);
        QCOMPARE(chunks(doc, 1, 0), (QStringList{"conflict 1 2 1 2"}));
        QCOMPARE(chunks(doc, 1, 2), (QStringList{"conflict 1 2 1 2", "replace 3 4 3 4"}));
        QCOMPARE(doc->differ()->conflicts, (std::vector<int>{0}));

        doc->moveCursor(1, 0);
        settle();
        QVERIFY(doc->action(QStringLiteral("file-next-conflict"))->isEnabled());
        trigger(doc, "file-next-conflict");
        QCOMPARE(doc->textView(1)->textCursor().blockNumber(), 1);
    }

    void mergeAllNonConflicting()
    {
        FileDiff *doc = open({writeFile("a\nX\nc\nd\n"), writeFile("a\nb\nc\nd\n"), writeFile("a\nb\nc\nD\n")});
        trigger(doc, "merge-all");
        QCOMPARE(doc->textBuffer(1)->text(), QStringLiteral("a\nX\nc\nD\n"));
        // The middle pane now has both sides' changes
        QCOMPARE(chunks(doc, 1, 0), (QStringList{"replace 3 4 3 4"}));
        QCOMPARE(chunks(doc, 1, 2), (QStringList{"replace 1 2 1 2"}));

        trigger(doc, "undo");
        QCOMPARE(doc->textBuffer(1)->text(), QStringLiteral("a\nb\nc\nd\n"));
    }

    void mergeAllFromOneSide()
    {
        FileDiff *doc = open({writeFile("a\nX\nc\nd\n"), writeFile("a\nb\nc\nd\n"), writeFile("a\nb\nc\nD\n")});
        doc->moveCursor(1, 0);
        settle();
        trigger(doc, "merge-all-left");
        QCOMPARE(doc->textBuffer(1)->text(), QStringLiteral("a\nX\nc\nd\n"));
        QVERIFY(chunks(doc, 1, 0).isEmpty());

        trigger(doc, "undo");
        trigger(doc, "merge-all-right");
        QCOMPARE(doc->textBuffer(1)->text(), QStringLiteral("a\nb\nc\nD\n"));
        QVERIFY(chunks(doc, 1, 2).isEmpty());
    }

    void autoMerge()
    {
        const QString output = dir_.filePath(QStringLiteral("merged.txt"));
        FileDiff *doc = window_->appendFilemerge(
            {writeFile("a\nX\nc\nd\n"), writeFile("a\nb\nc\nd\n"), writeFile("a\nb\nc\nD\n")}, output);
        settle();
        QCOMPARE(doc->comparisonMode(), FileComparisonMode::AutoMerge);
        QCOMPARE(doc->textBuffer(1)->text(), QStringLiteral("a\nX\nc\nD\n"));
        QVERIFY(doc->textBuffer(1)->isModified());
        QCOMPARE(doc->comparison()->type, RecentType::Merge);

        // Saving the middle pane writes the merge output file
        QVERIFY(doc->saveFile(1));
        QCOMPARE(readFile(output), QByteArray("a\nX\nc\nD\n"));
    }

    void autoMergeMarksConflicts()
    {
        FileDiff *doc = window_->appendFilemerge({writeFile("a\nX\n"), writeFile("a\nb\n"), writeFile("a\nY\n")});
        settle();
        QVERIFY(doc->textBuffer(1)->text().contains(QStringLiteral("(\?\?)")));
        QCOMPARE(doc->differ()->conflicts.size(), size_t(1));
    }

    void swapPanes()
    {
        FileDiff *doc = open({writeFile("left\n"), writeFile("right\n")});
        trigger(doc, "swap-2-panes");
        QCOMPARE(doc->textBuffer(0)->text(), QStringLiteral("right\n"));
        QCOMPARE(doc->textBuffer(1)->text(), QStringLiteral("left\n"));
        QCOMPARE(chunks(doc), (QStringList{"replace 0 1 0 1"}));
    }

    void textFiltersHideChanges()
    {
        MeldSettings *settings = MeldSettings::instance();
        const QList<FilterSetting> original = settings->filterSettings(QStringLiteral("text-filters"));
        settings->setFilterSettings(QStringLiteral("text-filters"),
                                    {{QStringLiteral("Script comment"), true, QStringLiteral("#.*")}});

        FileDiff *doc = open({writeFile("a = 1  # one\nb\n"), writeFile("a = 1  # uno\nc\n")});
        QCOMPARE(chunks(doc), (QStringList{"replace 1 2 1 2"}));

        // Toggling the filter off from the filter menu recompares
        trigger(doc, "text-custom-filter-0");
        QCOMPARE(chunks(doc), (QStringList{"replace 0 2 0 2"}));

        settings->setFilterSettings(QStringLiteral("text-filters"), original);
    }

    void labels()
    {
        FileDiff *doc = open({writeFile("a\n"), writeFile("b\n")});
        doc->setLabels({QStringLiteral("Mine"), QStringLiteral("Theirs")});
        QCOMPARE(doc->tabTitle(), QStringLiteral("Mine — Theirs"));
    }

    void reloadAfterExternalChange()
    {
        const QString left = writeFile("a\n");
        const QString right = writeFile("b\n");
        FileDiff *doc = open({left, right});
        QFile file(right);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("a\n");
        file.close();
        trigger(doc, "refresh");
        // Refresh only recompares; reverting reloads from disk
        QCOMPARE(doc->textBuffer(1)->text(), QStringLiteral("b\n"));
        doc->textBuffer(1)->setModified(false);
        doc->setFiles({left, right});
        settle();
        QCOMPARE(doc->textBuffer(1)->text(), QStringLiteral("a\n"));
        QVERIFY(chunks(doc).isEmpty());
    }

    void closeRemovesTheTab()
    {
        FileDiff *doc = open({writeFile("a\n"), writeFile("b\n")});
        QSignalSpy closed(doc, &MeldDoc::closed);
        doc->requestClose();
        QCOMPARE(closed.count(), 1);
        QVERIFY(!window_->hasPages());
    }

    void closingWhileComparing()
    {
        // Close tabs at different stages of loading, comparing and inline
        // highlighting (which runs in a worker thread)
        QByteArray left;
        QByteArray right;
        for (int i = 0; i < 3000; ++i) {
            const QByteArray line = "line " + QByteArray::number(i) + " some text here";
            left += line + '\n';
            right += (i % 7 == 0 ? line + " changed" : line) + '\n';
        }
        const QString leftFile = writeFile(left);
        const QString rightFile = writeFile(right);
        for (int iterations : {0, 1, 2, 5, 20, 1000}) {
            FileDiff *doc = window_->appendFilediff({leftFile, rightFile});
            for (int i = 0; i < iterations && window_->scheduler().tasksPending(); ++i)
                window_->scheduler().iteration();
            QPointer<FileDiff> guard(doc);
            doc->requestClose();
            QTRY_VERIFY(!guard);
        }
        // Let any outstanding worker results arrive
        QTest::qWait(200);
        QVERIFY(!window_->hasPages());
    }

    void closingBeforeLoadingDoesntPrompt()
    {
        // A new comparison has nothing to save, even before files load
        FileDiff *doc = window_->appendFilediff({writeFile("a\n"), writeFile("b\n")});
        QVERIFY(!doc->textBuffer(0)->isModified());
        QVERIFY(!doc->tabTitle().contains(QLatin1Char('*')));
        QSignalSpy closed(doc, &MeldDoc::closed);
        doc->requestClose();
        QCOMPARE(closed.count(), 1);
    }

    void displayChangesDontModifyBuffers()
    {
        FileDiff *doc = open({writeFile("a\tb\n"), writeFile("a b\n")});
        trigger(doc, "zoom-in");
        trigger(doc, "zoom-out");
        for (const char *name : {"draw-spaces-bool", "wrap-mode-bool", "show-line-numbers"}) {
            QAction *action = doc->action(QLatin1String(name));
            QVERIFY2(action, name);
            action->trigger();
            settle();
        }
        for (int pane = 0; pane < 2; ++pane) {
            QVERIFY(!doc->textBuffer(pane)->isModified());
            QVERIFY(!doc->textBuffer(pane)->document()->isUndoAvailable());
        }
        QVERIFY(!doc->action(QStringLiteral("undo"))->isEnabled());
    }
};

QTEST_MAIN(TestFileDiff)
#include "tst_filediff.moc"
