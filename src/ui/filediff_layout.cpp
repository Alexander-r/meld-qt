#include "filediff.h"

#include "chunkmap.h"
#include "core/merge.h"
#include "core/syntax/language.h"
#include "findbar.h"
#include "icons.h"
#include "inlinematcher.h"
#include "msgarea.h"
#include "pathlabel.h"
#include "settings.h"
#include "sourceview.h"
#include "statusbar.h"
#include "style.h"

#include <QApplication>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QPainter>
#include <QScrollBar>
#include <QTextBlock>
#include <QToolButton>
#include <QVBoxLayout>

namespace meld {

namespace {

//! A bar with a border line, like the GtkActionBar and status bar styling
class BorderedBar : public QWidget {
public:
    enum class Edge {
        Top,
        Bottom,
    };

    BorderedBar(Edge edge, QWidget *parent = nullptr)
        : QWidget(parent)
        , edge_(edge)
    {
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setPen(palette().color(QPalette::Mid));
        const int y = edge_ == Edge::Top ? 0 : height() - 1;
        painter.drawLine(0, y, width(), y);
    }

private:
    Edge edge_;
};

//! Holds the overview chunk maps, with a border on the left
class OverviewBox : public QWidget {
public:
    using QWidget::QWidget;

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), palette().color(QPalette::Window));
        painter.setPen(palette().color(QPalette::Mid));
        painter.drawLine(0, 0, 0, height());
    }
};

} // namespace

FileDiff::FileDiff(int numPanes, FileComparisonMode mode, QWidget *parent)
    : MeldDoc(parent)
    , comparisonMode_(mode)
    , cachedMatch_(new CachedSequenceMatcher(this))
{
    MeldSettings *settings = MeldSettings::instance();
    ignoreBlankLines_ = settings->boolValue(QStringLiteral("ignore-blank-lines"));
    showOverviewMap_ = settings->boolValue(QStringLiteral("show-overview-map"));

    if (mode == FileComparisonMode::AutoMerge)
        linediffer_ = std::make_unique<AutoMergeDiffer>();
    else
        linediffer_ = std::make_unique<Differ>();

    for (size_t pane = 0; pane < 3; ++pane) {
        textbuffer_[pane] = new MeldBuffer(this);
        undosequence_.addDocument(textbuffer_[pane]->document());
        bufferTexts_[pane] = std::make_unique<BufferLines>(textbuffer_[pane]);
        bufferFiltered_[pane] = std::make_unique<BufferLines>(
            textbuffer_[pane], [this, pane](const QString &text, int start, int end) {
                return filterText(int(pane), text, start, end);
            });
    }

    buildLayout();
    createActions();
    createTextFilters();

    for (int pane = 0; pane < 3; ++pane) {
        MeldBuffer *buffer = textbuffer_[size_t(pane)];
        SourceView *view = textview_[size_t(pane)];
        connect(buffer, &MeldBuffer::linesChanged, this, [this, pane](int start, int removed, int added) {
            onLinesChanged(pane, start, removed, added);
        });
        connect(buffer->document(), &QTextDocument::modificationChanged, this, [this]() { recomputeLabel(); });
        connect(view, &QPlainTextEdit::cursorPositionChanged, this, [this, pane]() {
            onCursorPositionChanged(pane);
            updateStatusbarCursor(pane);
        });
        connect(view, &QPlainTextEdit::selectionChanged, this, &FileDiff::updateTextActionsSensitivity);
        connect(view, &SourceView::focusChanged, this, [this, view](bool focused) {
            if (focused) {
                setFocusedTextview(view);
            } else {
                setFocusedTextview(nullptr);
            }
        });
        connect(view, &SourceView::undoRequested, this, &FileDiff::undo);
        connect(view, &SourceView::redoRequested, this, &FileDiff::redo);
        connect(view, &SourceView::escapePressed, this, [this]() {
            if (findbar_->isVisible())
                findbar_->hideBar();
        });
        connect(view, &SourceView::contextMenuRequested, this,
                [this, pane](const QPoint &pos) { showContextMenu(pane, pos); });
        connect(view, &SourceView::filesDropped, this,
                [this, pane](const QStringList &files) { dropFiles(pane, files); });
        connect(view->verticalScrollBar(), &QScrollBar::valueChanged, this, [this, pane]() { syncVScroll(pane); });
        connect(view->horizontalScrollBar(), &QScrollBar::valueChanged, this, [this, pane]() { syncHScroll(pane); });

        view->chunkIter = [this, pane](int lo, int hi) {
            return linediffer_->singleChanges(pane, std::pair{lo, hi});
        };
        view->currentChunkCheck = [this, pane](const DiffChunk &change) {
            const std::optional<int> chunk = linediffer_->locateChunk(pane, change.start_a).chunk;
            return chunk.has_value() && chunk == cursor_.chunk;
        };
        // The gutter shows chunks for this pane relative to the pane to its
        // left (or the middle pane, for the left pane)
        view->gutterChunkAt = [this, pane](int line) -> std::optional<DiffChunk> {
            const std::optional<int> index = linediffer_->locateChunk(pane, line).chunk;
            if (!index)
                return std::nullopt;
            return linediffer_->getChunk(*index, pane, pane - 1);
        };
        view->syncpointLines = [this, pane]() { return syncpoints_.lines(pane); };

        connect(statusbar_[size_t(pane)], &StatusBar::goToLine, this, [this, pane](int line) {
            if (cursor_.pane == pane && cursor_.line == line)
                return;
            moveCursor(pane, line, false);
        });
        // As the original's binding of the buffer's and status bar's languages
        connect(statusbar_[size_t(pane)], &StatusBar::languageSelected, this, [this, pane](const QString &id) {
            textbuffer_[size_t(pane)]->setLanguage(syntax::LanguageManager::instance().language(id));
        });
        connect(textbuffer_[size_t(pane)], &MeldBuffer::languageChanged, this, [this, pane]() {
            const syntax::Language *language = textbuffer_[size_t(pane)]->language();
            statusbar_[size_t(pane)]->setLanguage(language ? language->id() : QString());
        });
        connect(statusbar_[size_t(pane)], &StatusBar::encodingChanged, this, [this, pane](const QByteArray &encoding) {
            MeldBuffer *buf = textbuffer_[size_t(pane)];
            confirmUnsavedChangeAction([this, pane, encoding, buf]() { setFile(pane, buf->data.file, encoding); },
                                       {pane});
        });
    }

    for (ActionGutter *gutter : actiongutter_) {
        connect(gutter, &ActionGutter::chunkActionActivated, this, &FileDiff::onChunkActionActivated);
    }

    connect(linediffer_.get(), &Differ::diffsChanged, this, &FileDiff::onDiffsChanged);
    connect(&undosequence_, &UndoSequence::canUndoChanged, this,
            [this](bool can) { setActionEnabled(QStringLiteral("undo"), can); });
    connect(&undosequence_, &UndoSequence::canRedoChanged, this,
            [this](bool can) { setActionEnabled(QStringLiteral("redo"), can); });
    connect(this, &FileDiff::nextConflictChanged, this, [this](bool havePrev, bool haveNext) {
        setActionEnabled(QStringLiteral("file-previous-conflict"), havePrev);
        setActionEnabled(QStringLiteral("file-next-conflict"), haveNext);
    });
    connect(&watcher_, &QFileSystemWatcher::fileChanged, this, &FileDiff::onWatchedFileChanged);

    connect(settings, &MeldSettings::textFiltersChanged, this, [this]() {
        if (createTextFilters())
            refreshComparison();
        Q_EMIT filterActionsChanged();
    });
    connect(settings, &MeldSettings::changed, this, [this](const QString &key) {
        MeldSettings *s = MeldSettings::instance();
        if (key == QLatin1String("font") || key == QLatin1String("indent-width")) {
            for (SourceView *view : textview_)
                view->applyFontSettings();
            queueDraw();
        } else if (key == QLatin1String("ignore-blank-lines")) {
            ignoreBlankLines_ = s->boolValue(key);
            refreshComparison();
        } else if (key == QLatin1String("show-overview-map")) {
            action(QStringLiteral("show-overview-map"))->setChecked(s->boolValue(key));
        } else if (key == QLatin1String("show-line-numbers")) {
            action(QStringLiteral("show-line-numbers"))->setChecked(s->boolValue(key));
        } else if (key == QLatin1String("enable-space-drawer")) {
            action(QStringLiteral("draw-spaces-bool"))->setChecked(s->boolValue(key));
        } else if (key == QLatin1String("highlight-current-line")) {
            action(QStringLiteral("highlight-current-line-local"))->setChecked(s->boolValue(key));
        } else if (key == QLatin1String("wrap-mode")) {
            const bool wrap = s->stringValue(key) != QLatin1String("none");
            QAction *wrapAction = action(QStringLiteral("wrap-mode-bool"));
            if (wrapAction->isChecked() == wrap) {
                for (SourceView *view : textview_) {
                    view->setWrapText(!wrap);
                    view->setWrapText(wrap);
                }
            }
            wrapAction->setChecked(wrap);
        }
    });

    QList<QTextDocument *> documents;
    for (int pane = 0; pane < numPanes; ++pane)
        documents.append(textbuffer_[size_t(pane)]->document());
    syncpoints_.setDocuments(documents);

    for (int pane = 0; pane < 3; ++pane)
        setBufferEditable(pane, true);
    setNumPanes(numPanes);
    updateTextActionsSensitivity();
    setActionEnabled(QStringLiteral("undo"), false);
    setActionEnabled(QStringLiteral("redo"), false);
    onCurrentDiffChanged();

    qApp->installEventFilter(this);
}

FileDiff::~FileDiff()
{
    qApp->removeEventFilter(this);
    cachedMatch_->stop();
    // The views reference the buffers' documents; detach them before the
    // buffers are destroyed.
    for (SourceView *view : textview_) {
        view->chunkIter = {};
        view->currentChunkCheck = {};
        view->gutterChunkAt = {};
        view->syncpointLines = {};
    }
}

QWidget *FileDiff::buildFileToolbar(int pane)
{
    auto *bar = new BorderedBar(BorderedBar::Edge::Bottom, this);
    auto *layout = new QHBoxLayout(bar);
    layout->setContentsMargins(6, 4, 6, 5);
    layout->setSpacing(4);

    auto *readonly = new QToolButton(bar);
    readonly->setCheckable(true);
    readonly->setAutoRaise(true);
    readonly->setIcon(icons::icon(QStringLiteral("changes-prevent-symbolic")));
    readonly->setToolTip(tr("This file can not be written to. You may click here to unlock this file and make "
                            "changes anyway, but these changes must be saved to a new file."));
    readonly->hide();
    connect(readonly, &QToolButton::toggled, this,
            [this, pane](bool checked) { setBufferEditable(pane, !checked); });
    readonlyToggle_[size_t(pane)] = readonly;

    auto *save = new QToolButton(bar);
    save->setAutoRaise(true);
    save->setIcon(icons::icon(QStringLiteral("document-save-symbolic")));
    save->setToolTip(tr("Save file in this pane"));
    connect(save, &QToolButton::clicked, this, [this, pane]() { saveFile(pane); });
    fileSaveButton_[size_t(pane)] = save;

    auto *left = new QWidget(bar);
    auto *leftLayout = new QHBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(2);
    leftLayout->addWidget(readonly);
    leftLayout->addWidget(save);

    auto *center = new QWidget(bar);
    auto *centerLayout = new QHBoxLayout(center);
    centerLayout->setContentsMargins(0, 0, 0, 0);
    centerLayout->setSpacing(0);
    auto *open = new FileButton(pane, FileButton::Mode::OpenFile, tr("Select File"), center);
    open->setToolTip(tr("Open file in this pane"));
    connect(open, &FileButton::fileSelected, this, [this](int p, const QString &file) {
        confirmUnsavedChangeAction([this, p, file]() { setFile(p, file); }, {p});
    });
    fileOpenButton_[size_t(pane)] = open;
    auto *label = new PathLabel(center);
    label->setEmptyLabel(tr("File %1").arg(pane + 1));
    filelabel_[size_t(pane)] = label;
    centerLayout->addWidget(open);
    centerLayout->addWidget(label, 1);
    center->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);

    // Keep the centre widget centred, as GtkActionBar does
    auto *right = new QWidget(bar);
    right->setFixedWidth(left->sizeHint().width());
    layout->addWidget(left);
    layout->addStretch(1);
    layout->addWidget(center, 100);
    layout->addStretch(1);
    layout->addWidget(right);
    return bar;
}

void FileDiff::buildLayout()
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    grid_ = new QGridLayout;
    grid_->setContentsMargins(0, 0, 0, 0);
    grid_->setSpacing(0);
    outer->addLayout(grid_);

    static constexpr int paneColumns[3] = {0, 4, 8};
    for (int pane = 0; pane < 3; ++pane) {
        fileToolbar_[size_t(pane)] = buildFileToolbar(pane);
        grid_->addWidget(fileToolbar_[size_t(pane)], 0, paneColumns[pane]);

        auto *vbox = new QWidget(this);
        auto *vboxLayout = new QVBoxLayout(vbox);
        vboxLayout->setContentsMargins(0, 0, 0, 0);
        vboxLayout->setSpacing(0);
        msgarea_[size_t(pane)] = new MsgAreaController(vbox);
        vboxLayout->addWidget(msgarea_[size_t(pane)]);
        auto *view = new SourceView(vbox);
        view->setBuffer(textbuffer_[size_t(pane)]);
        view->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        textview_[size_t(pane)] = view;
        vboxLayout->addWidget(view, 1);
        vbox_[size_t(pane)] = vbox;
        grid_->addWidget(vbox, 1, paneColumns[pane]);
        grid_->setColumnStretch(paneColumns[pane], 1);

        auto *statusBorder = new BorderedBar(BorderedBar::Edge::Top, this);
        auto *statusLayout = new QHBoxLayout(statusBorder);
        statusLayout->setContentsMargins(0, 1, 0, 0);
        statusbar_[size_t(pane)] = new StatusBar(statusBorder);
        statusLayout->addWidget(statusbar_[size_t(pane)]);
        grid_->addWidget(statusBorder, 3, paneColumns[pane]);
    }

    for (int i = 0; i < 2; ++i) {
        dummyStatusbar_[size_t(i)] = new BorderedBar(BorderedBar::Edge::Top, this);
        grid_->addWidget(dummyStatusbar_[size_t(i)], 3, 1 + i * 4, 1, 3);
    }

    static constexpr int gutterColumns[4] = {1, 3, 5, 7};
    static constexpr int gutterViews[4][2] = {{0, 1}, {1, 0}, {1, 2}, {2, 1}};
    for (int i = 0; i < 4; ++i) {
        auto *gutter = new ActionGutter(i % 2 == 0 ? Qt::LeftToRight : Qt::RightToLeft, this);
        gutter->setViews(textview_[size_t(gutterViews[i][0])], textview_[size_t(gutterViews[i][1])]);
        actiongutter_[size_t(i)] = gutter;
        grid_->addWidget(gutter, 1, gutterColumns[i]);
    }

    for (int i = 0; i < 2; ++i) {
        auto *linkmap = new LinkMap(this);
        linkmap_[size_t(i)] = linkmap;
        grid_->addWidget(linkmap, 1, 2 + i * 4);
        connect(linkmap, &LinkMap::scrollRequested, this, [this](bool down) { nextDiff(down, false, true); });
    }

    chunkmapBox_ = new OverviewBox(this);
    auto *chunkmapLayout = new QHBoxLayout(chunkmapBox_);
    chunkmapLayout->setContentsMargins(3, 0, 2, 0);
    chunkmapLayout->setSpacing(2);
    for (int pane = 0; pane < 3; ++pane) {
        auto *chunkmap = new TextViewChunkMap(textview_[size_t(pane)], chunkmapBox_);
        chunkmap_[size_t(pane)] = chunkmap;
        chunkmapLayout->addWidget(chunkmap);
    }
    for (int pane = 0; pane < 3; ++pane) {
        QList<SourceView *> paired;
        for (int other = 0; other < 3; ++other) {
            if (other != pane)
                paired.append(textview_[size_t(other)]);
        }
        chunkmap_[size_t(pane)]->setPairedViews(paired);
    }
    grid_->addWidget(chunkmapBox_, 1, 9);
    overviewToolbarSpacer_ = new BorderedBar(BorderedBar::Edge::Bottom, this);
    grid_->addWidget(overviewToolbarSpacer_, 0, 9);
    overviewStatusSpacer_ = new BorderedBar(BorderedBar::Edge::Top, this);
    grid_->addWidget(overviewStatusSpacer_, 3, 9);

    findbar_ = new FindBar(this);
    findbar_->hide();
    findbar_->setTextViews({textview_[0], textview_[1], textview_[2]});
    grid_->addWidget(findbar_, 2, 0, 1, 10);

    for (SourceView *view : textview_) {
        for (LinkMap *linkmap : linkmap_)
            connect(view, &SourceView::viewportChanged, linkmap, qOverload<>(&QWidget::update));
        for (ActionGutter *gutter : actiongutter_)
            connect(view, &SourceView::viewportChanged, gutter, qOverload<>(&QWidget::update));
    }
}

void FileDiff::createActions()
{
    MeldSettings *settings = MeldSettings::instance();

    // Display options shared between all panes (the "view-local" actions)
    const auto addViewLocal = [this](const QString &name, const QString &label, bool initial,
                                     void (SourceView::*setter)(bool)) {
        QAction *a = addToggleAction(name, initial, [this, setter](bool checked) {
            for (SourceView *view : textview_)
                (view->*setter)(checked);
        });
        a->setText(label);
        for (SourceView *view : textview_)
            (view->*setter)(initial);
        viewLocalActions_.append(a);
    };
    addViewLocal(QStringLiteral("show-line-numbers"), tr("Show line numbers"),
                 settings->boolValue(QStringLiteral("show-line-numbers")), &SourceView::setShowLineNumbers);
    addViewLocal(QStringLiteral("draw-spaces-bool"), tr("Show whitespace"),
                 settings->boolValue(QStringLiteral("enable-space-drawer")), &SourceView::setDrawSpaces);
    addViewLocal(QStringLiteral("highlight-current-line-local"), tr("Highlight current line"),
                 settings->boolValue(QStringLiteral("highlight-current-line")), &SourceView::setHighlightCurrentLine);
    addViewLocal(QStringLiteral("wrap-mode-bool"), tr("Text wrapping"),
                 settings->stringValue(QStringLiteral("wrap-mode")) != QLatin1String("none"), &SourceView::setWrapText);
    for (StatusBar *statusbar : statusbar_)
        statusbar->setDisplayActions(viewLocalActions_);

    addToggleAction(QStringLiteral("show-overview-map"), showOverviewMap_, [this](bool show) {
        showOverviewMap_ = show;
        chunkmapBox_->setVisible(show);
        overviewToolbarSpacer_->setVisible(show);
        overviewStatusSpacer_->setVisible(show);
    });
    addToggleAction(QStringLiteral("lock-scrolling"), false, [this](bool lock) { lockScrolling_ = lock; });

    addAction(QStringLiteral("add-sync-point"), [this]() {
        if (const int pane = focusedPane(); pane != -1) {
            syncpoints_.add(pane, textview_[size_t(pane)]->textCursor().position());
            refreshSyncPoints();
        }
    });
    addAction(QStringLiteral("move-sync-point"), [this]() {
        if (const int pane = focusedPane(); pane != -1) {
            syncpoints_.move(pane, textview_[size_t(pane)]->textCursor().position());
            refreshSyncPoints();
        }
    });
    // Match duplicates add-sync-point for labeling reasons
    addAction(QStringLiteral("match-sync-point"), [this]() {
        if (const int pane = focusedPane(); pane != -1) {
            syncpoints_.add(pane, textview_[size_t(pane)]->textCursor().position());
            refreshSyncPoints();
        }
    });
    addAction(QStringLiteral("remove-sync-point"), [this]() {
        if (const int pane = focusedPane(); pane != -1) {
            syncpoints_.remove(pane, textview_[size_t(pane)]->textCursor().position());
            refreshSyncPoints();
        }
    });
    addAction(QStringLiteral("clear-sync-point"), [this]() {
        syncpoints_.clear();
        refreshSyncPoints();
    });
    addAction(QStringLiteral("copy"), [this]() {
        if (const int pane = focusedPane(); pane != -1)
            textview_[size_t(pane)]->copy();
    });
    addAction(QStringLiteral("copy-full-path"), [this]() { copyFullPath(); });
    addAction(QStringLiteral("cut"), [this]() {
        if (const int pane = focusedPane(); pane != -1) {
            SourceView *view = textview_[size_t(pane)];
            QTextCursor cursor = view->textCursor();
            cursor.beginEditBlock();
            view->cut();
            cursor.endEditBlock();
            view->ensureCursorVisible();
        }
    });
    addAction(QStringLiteral("file-previous-conflict"), [this]() { goToChunk(cursor_.prevConflict, cursor_.pane); });
    addAction(QStringLiteral("file-next-conflict"), [this]() { goToChunk(cursor_.nextConflict, cursor_.pane); });
    addAction(QStringLiteral("file-push-left"), [this]() { pushChange(-1); });
    addAction(QStringLiteral("file-push-right"), [this]() { pushChange(1); });
    addAction(QStringLiteral("file-pull-left"), [this]() { pullChange(-1); });
    addAction(QStringLiteral("file-pull-right"), [this]() { pullChange(1); });
    addAction(QStringLiteral("file-copy-left-up"), [this]() { copyChange(-1, true); });
    addAction(QStringLiteral("file-copy-right-up"), [this]() { copyChange(1, true); });
    addAction(QStringLiteral("file-copy-left-down"), [this]() { copyChange(-1, false); });
    addAction(QStringLiteral("file-copy-right-down"), [this]() { copyChange(1, false); });
    addAction(QStringLiteral("file-delete"), [this]() { deleteChange(); });
    addAction(QStringLiteral("find"), [this]() { findAction(false); });
    addAction(QStringLiteral("find-next"), [this]() { findbar_->startFindNext(focusPane_); });
    addAction(QStringLiteral("find-previous"), [this]() { findbar_->startFindPrevious(focusPane_); });
    addAction(QStringLiteral("find-replace"), [this]() { findAction(true); });
    addAction(QStringLiteral("format-as-patch"), [this]() { formatAsPatch(); });
    addAction(QStringLiteral("go-to-line"), [this]() {
        if (const int pane = focusedPane(); pane != -1)
            statusbar_[size_t(pane)]->startGoToLine();
    });
    addAction(QStringLiteral("merge-all-left"), [this]() {
        const auto [src, dst] = actionPanes(-1, true);
        pullAllNonConflictingChanges(src, dst);
    });
    addAction(QStringLiteral("merge-all-right"), [this]() {
        const auto [src, dst] = actionPanes(1, true);
        pullAllNonConflictingChanges(src, dst);
    });
    addAction(QStringLiteral("merge-all"), [this]() { mergeAllChanges(); });
    addAction(QStringLiteral("next-change"), [this]() { nextDiff(true); });
    addAction(QStringLiteral("next-change-shortcut"), [this]() { nextDiff(true); });
    addAction(QStringLiteral("next-pane"), [this]() {
        const int pane = focusedPane();
        if (pane != -1)
            moveCursorPane(pane, (pane + 1) % numPanes_);
    });
    addAction(QStringLiteral("open-external"), [this]() { openExternal(); });
    addAction(QStringLiteral("open-folder"), [this]() { openFolder(); });
    addAction(QStringLiteral("paste"), [this]() {
        if (const int pane = focusedPane(); pane != -1) {
            textview_[size_t(pane)]->paste();
            textview_[size_t(pane)]->ensureCursorVisible();
        }
    });
    addAction(QStringLiteral("previous-change"), [this]() { nextDiff(false); });
    addAction(QStringLiteral("previous-change-shortcut"), [this]() { nextDiff(false); });
    addAction(QStringLiteral("previous-pane"), [this]() {
        const int pane = focusedPane();
        if (pane != -1)
            moveCursorPane(pane, (pane - 1 + numPanes_) % numPanes_);
    });
    addAction(QStringLiteral("redo"), [this]() { redo(); });
    addAction(QStringLiteral("refresh"), [this]() { refreshComparison(); });
    addAction(QStringLiteral("revert"), [this]() { revert(); });
    addAction(QStringLiteral("save"), [this]() {
        if (const int pane = focusedPane(); pane != -1)
            saveFile(pane);
    });
    addAction(QStringLiteral("save-all"), [this]() {
        for (int i = 0; i < numPanes_; ++i) {
            if (textbuffer_[size_t(i)]->isModified())
                saveFile(i);
        }
    });
    addAction(QStringLiteral("save-as"), [this]() {
        if (const int pane = focusedPane(); pane != -1)
            saveFile(pane, true);
    });
    addAction(QStringLiteral("undo"), [this]() { undo(); });
    addAction(QStringLiteral("swap-2-panes"), [this]() { swapPanes(); });
    addAction(QStringLiteral("zoom-in"), []() { updateFontZoom(1); });
    addAction(QStringLiteral("zoom-out"), []() { updateFontZoom(-1); });
    addAction(QStringLiteral("zoom-reset"), []() { updateFontZoom(std::nullopt); });
    addToggleAction(QStringLiteral("text-filter"), false, {});

    chunkmapBox_->setVisible(showOverviewMap_);
    overviewToolbarSpacer_->setVisible(showOverviewMap_);
    overviewStatusSpacer_->setVisible(showOverviewMap_);
}

} // namespace meld
