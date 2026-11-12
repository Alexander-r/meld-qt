#include "meldwindow.h"

#include "accelerators.h"
#include "conflictcomparison.h"
#include "core/archive.h"
#include "core/patch.h"
#include "dirdiff.h"
#include "filediff.h"
#include "icons.h"
#include "newdifftab.h"
#include "patchcomparison.h"
#include "recent.h"
#include "settings.h"
#include "style.h"
#include "vcview.h"

#include <QApplication>
#include <QCloseEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QHeaderView>
#include <QLoggingCategory>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPointer>
#include <QProgressDialog>
#include <QTabBar>
#include <QTabWidget>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <stdexcept>

namespace meld {

Q_LOGGING_CATEGORY(lcWindow, "meld.window")

//! A small busy indicator (GtkSpinner)
class Spinner : public QWidget {
public:
    explicit Spinner(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setFixedSize(20, 20);
        timer_.setInterval(60);
        connect(&timer_, &QTimer::timeout, this, [this]() {
            angle_ = (angle_ + 30) % 360;
            update();
        });
        hide();
    }

    void start()
    {
        show();
        timer_.start();
    }

    void stop()
    {
        timer_.stop();
        hide();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        QPen pen(palette().color(QPalette::WindowText), 2.0, Qt::SolidLine, Qt::RoundCap);
        painter.setPen(pen);
        const QRectF bounds = QRectF(rect()).adjusted(3, 3, -3, -3);
        painter.drawArc(bounds, -angle_ * 16, 270 * 16);
    }

private:
    QTimer timer_;
    int angle_ = 0;
};

// ---------------------------------------------------------------------------
// ProxyAction

ProxyAction::ProxyAction(const QString &name, QObject *parent)
    : QAction(parent)
    , name_(name)
{
    setObjectName(QStringLiteral("view.") + name);
    setEnabled(false);
    shortcuts_ = shortcutsForAction(QStringLiteral("view.") + name);
    connect(this, &QAction::triggered, this, [this](bool checked) {
        if (!target_)
            return;
        if (target_->isCheckable())
            target_->setChecked(checked);
        else
            target_->trigger();
    });
}

void ProxyAction::bind(QAction *target)
{
    if (target_)
        disconnect(target_, nullptr, this, nullptr);
    target_ = target;
    if (!target) {
        setEnabled(false);
        if (isCheckable()) {
            const QSignalBlocker blocker(this);
            setChecked(false);
        }
        // Shortcuts are only active for the actions of the current tab, so
        // that e.g. Alt+Left works for both file and folder comparisons
        setShortcuts({});
        return;
    }
    setCheckable(target->isCheckable());
    {
        const QSignalBlocker blocker(this);
        setChecked(target->isChecked());
    }
    setEnabled(target->isEnabled());
    setShortcuts(shortcuts_);
    connect(target, &QAction::changed, this, [this]() {
        if (!target_)
            return;
        setEnabled(target_->isEnabled());
        const QSignalBlocker blocker(this);
        setChecked(target_->isChecked());
    });
    connect(target, &QAction::toggled, this, [this](bool checked) {
        const QSignalBlocker blocker(this);
        setChecked(checked);
    });
}

// ---------------------------------------------------------------------------
// MeldWindow

MeldWindow::MeldWindow()
{
    setWindowTitle(tr("Meld Qt"));
    setWindowIcon(QIcon(QStringLiteral(":/icons/meld-qt.svg")));
    setMinimumHeight(400);
    setAcceptDrops(true);

    tabs_ = new QTabWidget(this);
    tabs_->setDocumentMode(true);
    tabs_->setTabsClosable(true);
    tabs_->setMovable(true);
    tabs_->setElideMode(Qt::ElideMiddle);
    tabs_->tabBar()->setExpanding(true);
    // Like AdwTabBar, only show tabs when there is more than one
    tabs_->tabBar()->setAutoHide(true);
    setCentralWidget(tabs_);

    connect(tabs_, &QTabWidget::currentChanged, this, &MeldWindow::onCurrentTabChanged);
    connect(tabs_, &QTabWidget::tabCloseRequested, this, [this](int index) {
        if (auto *doc = qobject_cast<MeldDoc *>(tabs_->widget(index)))
            doc->requestClose();
    });

    buildHeaderBar();

    scheduler_.connectRunnable(this, [this](SchedulerBase *) { onSchedulerRunnable(); });
    idleTimer_.setInterval(0);
    connect(&idleTimer_, &QTimer::timeout, this, &MeldWindow::onIdle);

    MeldSettings *settings = MeldSettings::instance();
    connect(settings, &MeldSettings::changed, this, [this](const QString &key) {
        if (key == QLatin1String("recent-comparisons"))
            rebuildRecentMenu();
    });

    restoreWindowState();
    onCurrentTabChanged();
}

MeldWindow::~MeldWindow()
{
    for (MeldDoc *doc : docs())
        scheduler_.removeScheduler(&doc->scheduler());
}

void MeldWindow::restoreWindowState()
{
    MeldSettings *settings = MeldSettings::instance();
    const int width = settings->intValue(QStringLiteral("window-width"));
    const int height = settings->intValue(QStringLiteral("window-height"));
    resize(width > 0 ? width : 1100, height > 0 ? height : 760);
    if (settings->boolValue(QStringLiteral("window-is-maximized")))
        setWindowState(windowState() | Qt::WindowMaximized);
}

void MeldWindow::saveWindowState()
{
    MeldSettings *settings = MeldSettings::instance();
    const bool maximized = isMaximized();
    settings->setValue(QStringLiteral("window-is-maximized"), maximized);
    if (!maximized && !isFullScreen()) {
        settings->setValue(QStringLiteral("window-width"), width());
        settings->setValue(QStringLiteral("window-height"), height());
    }
}

ProxyAction *MeldWindow::proxy(const QString &name, const QString &text, const QString &icon)
{
    ProxyAction *action = proxies_.value(name);
    if (!action) {
        action = new ProxyAction(name, this);
        action->setShortcutContext(Qt::WindowShortcut);
        addAction(action);
        proxies_.insert(name, action);
    }
    if (!text.isEmpty())
        action->setText(text);
    if (!icon.isEmpty())
        action->setIcon(icons::icon(icon));
    return action;
}

QAction *MeldWindow::addWindowAction(const QString &name, const QString &text, std::function<void()> callback)
{
    auto *action = new QAction(text, this);
    action->setObjectName(name);
    QList<QKeySequence> shortcuts = shortcutsForAction(name);
    action->setShortcuts(shortcuts);
    action->setShortcutContext(Qt::WindowShortcut);
    connect(action, &QAction::triggered, this, [callback = std::move(callback)]() { callback(); });
    addAction(action);
    windowActions_.insert(name, action);
    return action;
}

QToolButton *MeldWindow::toolButton(QAction *action)
{
    auto *button = new QToolButton(headerBar_);
    button->setDefaultAction(action);
    button->setAutoRaise(true);
    button->setFocusPolicy(Qt::NoFocus);
    return button;
}

void MeldWindow::buildHeaderBar()
{
    headerBar_ = new QToolBar(tr("Header bar"), this);
    headerBar_->setObjectName(QStringLiteral("meldHeaderBar"));
    headerBar_->setMovable(false);
    headerBar_->setFloatable(false);
    headerBar_->setIconSize(QSize(18, 18));
    headerBar_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    headerBar_->setContextMenuPolicy(Qt::PreventContextMenu);
    addToolBar(Qt::TopToolBarArea, headerBar_);

    // Window and application actions
    QAction *newTab = addWindowAction(QStringLiteral("win.new-tab"), tr("New Comparison"),
                                      [this]() { appendNewComparison(); });
    newTab->setIcon(icons::icon(QStringLiteral("tab-new-symbolic")));
    newTab->setToolTip(tr("Start a new comparison"));
    addWindowAction(QStringLiteral("win.close"), tr("Close"), [this]() {
        if (MeldDoc *doc = currentDoc())
            doc->requestClose();
    });
    QAction *stop = addWindowAction(QStringLiteral("win.stop"), tr("&Stop"), [this]() {
        if (MeldDoc *doc = currentDoc())
            doc->actionStop();
    });
    stop->setEnabled(false);
    QAction *fullscreen = addWindowAction(QStringLiteral("win.fullscreen"), tr("Fullscreen"), [this]() {
        if (isFullScreen())
            showNormal();
        else
            showFullScreen();
    });
    fullscreen->setCheckable(true);
    addWindowAction(QStringLiteral("app.quit"), tr("Quit"), [this]() { Q_EMIT quitRequested(); });
    addWindowAction(QStringLiteral("app.preferences"), tr("&Preferences"), [this]() { Q_EMIT preferencesRequested(); });
    addWindowAction(QStringLiteral("app.about"), tr("&About Meld Qt"), [this]() { Q_EMIT aboutRequested(); });
    addWindowAction(QStringLiteral("win.show-help-overlay"), tr("Keyboard Shortcuts"),
                    [this]() { showKeyboardShortcuts(); });

    // Tab navigation, as provided by AdwTabView in the original
    const auto selectTab = [this](int index) {
        if (index >= 0 && index < tabs_->count())
            tabs_->setCurrentIndex(index);
    };
    const auto moveTab = [this](int offset) {
        const int index = tabs_->currentIndex();
        const int target = index + offset;
        if (index >= 0 && target >= 0 && target < tabs_->count())
            tabs_->tabBar()->moveTab(index, target);
    };
    addWindowAction(QStringLiteral("win.previous-tab"), tr("Previous Tab"),
                    [this, selectTab]() { selectTab(tabs_->currentIndex() - 1); });
    addWindowAction(QStringLiteral("win.next-tab"), tr("Next Tab"),
                    [this, selectTab]() { selectTab(tabs_->currentIndex() + 1); });
    addWindowAction(QStringLiteral("win.move-tab-left"), tr("Move Tab Left"), [moveTab]() { moveTab(-1); });
    addWindowAction(QStringLiteral("win.move-tab-right"), tr("Move Tab Right"), [moveTab]() { moveTab(1); });
    for (int i = 0; i < 10; ++i) {
        // Alt+1 to Alt+9 select the first nine tabs, and Alt+0 the tenth
        auto *action = new QAction(this);
        action->setShortcut(QKeySequence(Qt::ALT | Qt::Key(Qt::Key_0 + i)));
        action->setShortcutContext(Qt::WindowShortcut);
        connect(action, &QAction::triggered, this, [selectTab, i]() { selectTab(i == 0 ? 9 : i - 1); });
        addAction(action);
    }

    // Recent comparisons
    recentMenu_ = new QMenu(this);
    auto *recentButton = new QToolButton(headerBar_);
    recentButton->setIcon(icons::icon(QStringLiteral("document-open-recent-symbolic")));
    recentButton->setToolTip(tr("Open a recent comparison"));
    recentButton->setPopupMode(QToolButton::InstantPopup);
    recentButton->setAutoRaise(true);
    recentButton->setFocusPolicy(Qt::NoFocus);
    recentButton->setMenu(recentMenu_);
    rebuildRecentMenu();

    headerBar_->addWidget(toolButton(newTab));
    headerBar_->addWidget(recentButton);
    headerBar_->addSeparator();

    ProxyAction *previous = proxy(QStringLiteral("previous-change"), tr("Previous Change"),
                                  QStringLiteral("go-up-symbolic"));
    previous->setToolTip(tr("Go to the previous change"));
    ProxyAction *next = proxy(QStringLiteral("next-change"), tr("Next Change"), QStringLiteral("go-down-symbolic"));
    next->setToolTip(tr("Go to the next change"));
    headerBar_->addWidget(toolButton(previous));
    headerBar_->addWidget(toolButton(next));

    ProxyAction *previousConflict = proxy(QStringLiteral("file-previous-conflict"), tr("Previous Conflict"),
                                          QStringLiteral("go-top-symbolic"));
    previousConflict->setToolTip(tr("Go to the previous conflict"));
    ProxyAction *nextConflict = proxy(QStringLiteral("file-next-conflict"), tr("Next Conflict"),
                                      QStringLiteral("go-bottom-symbolic"));
    nextConflict->setToolTip(tr("Go to the next conflict"));
    conflictActions_.append(headerBar_->addSeparator());
    conflictActions_.append(headerBar_->addWidget(toolButton(previousConflict)));
    conflictActions_.append(headerBar_->addWidget(toolButton(nextConflict)));

    // View toolbar for file comparisons (filediff-actions.ui)
    fileDiffToolbar_.append(headerBar_->addSeparator());
    ProxyAction *pushLeft = proxy(QStringLiteral("file-push-left"), tr("Push to Left"),
                                  QStringLiteral("go-previous-symbolic"));
    pushLeft->setToolTip(tr("Push current change to the left"));
    ProxyAction *pushRight = proxy(QStringLiteral("file-push-right"), tr("Push to Right"),
                                   QStringLiteral("go-next-symbolic"));
    pushRight->setToolTip(tr("Push current change to the right"));
    fileDiffToolbar_.append(headerBar_->addWidget(toolButton(pushLeft)));
    fileDiffToolbar_.append(headerBar_->addWidget(toolButton(pushRight)));
    auto *copyMenu = new QMenu(this);
    copyMenu->addAction(proxy(QStringLiteral("file-copy-left-up"), tr("Copy change above the left chunk")));
    copyMenu->addAction(proxy(QStringLiteral("file-copy-left-down"), tr("Copy change below the left chunk")));
    copyMenu->addAction(proxy(QStringLiteral("file-copy-right-up"), tr("Copy change above the right chunk")));
    copyMenu->addAction(proxy(QStringLiteral("file-copy-right-down"), tr("Copy change below the right chunk")));
    auto *copyButton = new QToolButton(headerBar_);
    copyButton->setIcon(icons::icon(QStringLiteral("edit-copy-symbolic")));
    copyButton->setToolTip(tr("Copy chunks"));
    copyButton->setPopupMode(QToolButton::InstantPopup);
    copyButton->setAutoRaise(true);
    copyButton->setFocusPolicy(Qt::NoFocus);
    copyButton->setMenu(copyMenu);
    fileDiffToolbar_.append(headerBar_->addWidget(copyButton));
    ProxyAction *deleteChange = proxy(QStringLiteral("file-delete"), tr("Delete Change"),
                                      QStringLiteral("edit-delete-symbolic"));
    deleteChange->setToolTip(tr("Delete change"));
    fileDiffToolbar_.append(headerBar_->addWidget(toolButton(deleteChange)));

    // View toolbar for folder comparisons (dirdiff-actions.ui)
    dirDiffToolbar_.append(headerBar_->addSeparator());
    ProxyAction *copyLeft = proxy(QStringLiteral("folder-copy-left"), tr("Copy to Left"),
                                  QStringLiteral("go-previous-symbolic"));
    copyLeft->setToolTip(tr("Copy to left"));
    ProxyAction *copyRight = proxy(QStringLiteral("folder-copy-right"), tr("Copy to Right"),
                                   QStringLiteral("go-next-symbolic"));
    copyRight->setToolTip(tr("Copy to right"));
    ProxyAction *folderDelete = proxy(QStringLiteral("folder-delete"), tr("Delete"),
                                      QStringLiteral("user-trash-symbolic"));
    folderDelete->setToolTip(tr("Delete selected"));
    dirDiffToolbar_.append(headerBar_->addWidget(toolButton(copyLeft)));
    dirDiffToolbar_.append(headerBar_->addWidget(toolButton(copyRight)));
    dirDiffToolbar_.append(headerBar_->addWidget(toolButton(folderDelete)));

    auto *spacer = new QWidget(headerBar_);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    headerBar_->addWidget(spacer);

    spinner_ = new Spinner(headerBar_);
    headerBar_->addWidget(spinner_);

    textFilterMenu_ = new QMenu(this);
    textFilterButton_ = new QToolButton(headerBar_);
    textFilterButton_->setText(tr("Text Filters"));
    textFilterButton_->setPopupMode(QToolButton::InstantPopup);
    textFilterButton_->setToolButtonStyle(Qt::ToolButtonTextOnly);
    textFilterButton_->setFocusPolicy(Qt::NoFocus);
    textFilterButton_->setMenu(textFilterMenu_);
    textFilterButtonAction_ = headerBar_->addWidget(textFilterButton_);

    folderFilterMenu_ = new QMenu(this);
    folderFilterButton_ = new QToolButton(headerBar_);
    folderFilterButton_->setText(tr("File Filters"));
    folderFilterButton_->setPopupMode(QToolButton::InstantPopup);
    folderFilterButton_->setToolButtonStyle(Qt::ToolButtonTextOnly);
    folderFilterButton_->setFocusPolicy(Qt::NoFocus);
    folderFilterButton_->setMenu(folderFilterMenu_);
    folderFilterButtonAction_ = headerBar_->addWidget(folderFilterButton_);

    vcFilterMenu_ = new QMenu(this);
    vcFilterButton_ = new QToolButton(headerBar_);
    vcFilterButton_->setText(tr("Version Filters"));
    vcFilterButton_->setPopupMode(QToolButton::InstantPopup);
    vcFilterButton_->setToolButtonStyle(Qt::ToolButtonTextOnly);
    vcFilterButton_->setFocusPolicy(Qt::NoFocus);
    vcFilterButton_->setMenu(vcFilterMenu_);
    vcFilterButtonAction_ = headerBar_->addWidget(vcFilterButton_);

    // F8 opens whichever filter menu is shown
    auto *filterPopup = new QAction(this);
    filterPopup->setShortcuts(shortcutsForAction(QStringLiteral("view.text-filter")));
    connect(filterPopup, &QAction::triggered, this, [this]() {
        if (textFilterButtonAction_->isVisible())
            textFilterButton_->showMenu();
        else if (folderFilterButtonAction_->isVisible())
            folderFilterButton_->showMenu();
        else if (vcFilterButtonAction_->isVisible())
            vcFilterButton_->showMenu();
    });
    addAction(filterPopup);

    gearButton_ = new QToolButton(headerBar_);
    gearButton_->setIcon(icons::icon(QStringLiteral("open-menu-symbolic")));
    gearButton_->setToolTip(tr("Main Menu"));
    gearButton_->setPopupMode(QToolButton::InstantPopup);
    gearButton_->setAutoRaise(true);
    gearButton_->setFocusPolicy(Qt::NoFocus);
    gearButton_->setMenu(buildGearMenu());
    headerBar_->addWidget(gearButton_);
    addWindowAction(QStringLiteral("win.gear-menu"), tr("Main Menu"), [this]() { gearButton_->showMenu(); });

    // Proxies that are only reached through shortcuts or menus
    for (const char *name : {"next-change-shortcut", "previous-change-shortcut", "next-pane", "previous-pane",
                             "find-next", "find-previous", "go-to-line", "file-pull-left", "file-pull-right",
                             "undo", "redo", "save", "folder-compare", "swap-2-panes", "zoom-in", "zoom-out",
                             "zoom-reset", "vc-commit"}) {
        proxy(QLatin1String(name));
    }
}

QMenu *MeldWindow::buildGearMenu()
{
    auto *menu = new QMenu(this);
    menu->addAction(proxy(QStringLiteral("save-as"), tr("Save As…")));
    menu->addAction(proxy(QStringLiteral("save-all"), tr("Save A&ll")));
    menu->addAction(proxy(QStringLiteral("revert"), tr("Revert Files…")));
    menu->addAction(proxy(QStringLiteral("open-external"), tr("&Open Externally")));
    menu->addSeparator();
    menu->addAction(proxy(QStringLiteral("refresh"), tr("Refresh Comparison")));
    menu->addSeparator();
    menu->addAction(proxy(QStringLiteral("find"), tr("&Find…")));
    menu->addAction(proxy(QStringLiteral("find-replace"), tr("&Replace…")));
    menu->addSeparator();

    QMenu *view = menu->addMenu(tr("&View"));
    view->addAction(windowActions_.value(QStringLiteral("win.fullscreen")));
    view->addAction(proxy(QStringLiteral("show-overview-map"), tr("Overview Map")));
    view->addAction(proxy(QStringLiteral("vc-console-visible"), tr("Version Control Console")));
    view->addAction(proxy(QStringLiteral("lock-scrolling"), tr("Lock Scrolling")));
    view->addSeparator();
    view->addAction(proxy(QStringLiteral("zoom-in"), tr("Zoom In")));
    view->addAction(proxy(QStringLiteral("zoom-out"), tr("Zoom Out")));
    view->addAction(proxy(QStringLiteral("zoom-reset"), tr("Reset Zoom")));
    view->addSeparator();
    view->addAction(proxy(QStringLiteral("swap-2-panes"), tr("Swap Panes")));

    QMenu *comparison = menu->addMenu(tr("&Comparison"));
    comparison->addAction(windowActions_.value(QStringLiteral("win.stop")));
    comparison->addSeparator();
    comparison->addAction(proxy(QStringLiteral("merge-all-left"), tr("Merge All from &Left")));
    comparison->addAction(proxy(QStringLiteral("merge-all-right"), tr("Merge All from &Right")));
    comparison->addAction(proxy(QStringLiteral("merge-all"), tr("Merge &All")));
    comparison->addSeparator();
    comparison->addAction(proxy(QStringLiteral("format-as-patch"), tr("Format as &Patch…")));

    menu->addSeparator();
    menu->addAction(windowActions_.value(QStringLiteral("app.preferences")));
    menu->addAction(windowActions_.value(QStringLiteral("win.show-help-overlay")));
    menu->addAction(windowActions_.value(QStringLiteral("app.about")));
    connect(menu, &QMenu::aboutToShow, this, [this]() {
        windowActions_.value(QStringLiteral("win.fullscreen"))->setChecked(isFullScreen());
    });
    return menu;
}

void MeldWindow::rebuildRecentMenu()
{
    recentMenu_->clear();
    const QList<Comparison> recent = RecentComparisons::load();
    if (recent.isEmpty()) {
        QAction *empty = recentMenu_->addAction(tr("No recent comparisons"));
        empty->setEnabled(false);
        return;
    }
    for (const Comparison &comparison : recent) {
        QAction *action = recentMenu_->addAction(RecentComparisons::displayName(comparison));
        action->setToolTip(comparison.paths.join(QLatin1Char('\n')));
        connect(action, &QAction::triggered, this, [this, comparison]() {
            try {
                appendRecent(comparison);
            } catch (const std::exception &e) {
                qCWarning(lcWindow) << "Error opening recent comparison:" << e.what();
            }
        });
    }
    recentMenu_->addSeparator();
    recentMenu_->addAction(tr("Clear Recent Comparisons"), this, []() { RecentComparisons::clear(); });
}

void MeldWindow::updateFilterMenus()
{
    textFilterMenu_->clear();
    folderFilterMenu_->clear();
    vcFilterMenu_->clear();
    MeldDoc *doc = currentDoc();
    if (!doc)
        return;

    if (doc->action(QStringLiteral("vc-status-modified"))) {
        for (const char *name : {"vc-status-modified", "vc-status-normal", "vc-status-unknown", "vc-status-ignored"})
            vcFilterMenu_->addAction(doc->action(QLatin1String(name)));
        vcFilterMenu_->addSeparator();
        vcFilterMenu_->addAction(doc->action(QStringLiteral("vc-flatten")));
    }

    for (const auto &[name, label] : doc->textFilterActions()) {
        if (QAction *action = doc->action(name))
            textFilterMenu_->addAction(action);
    }

    const QList<QPair<QString, QString>> fileFilters = doc->fileFilterActions();
    if (doc->action(QStringLiteral("folder-status-same"))) {
        textFilterMenu_->clear();
        QAction *section = folderFilterMenu_->addSection(tr("File Status"));
        Q_UNUSED(section);
        for (const char *name : {"folder-status-same", "folder-status-new", "folder-status-modified"})
            folderFilterMenu_->addAction(doc->action(QLatin1String(name)));
        folderFilterMenu_->addSection(tr("Filename"));
        for (const auto &[name, label] : fileFilters) {
            if (QAction *action = doc->action(name))
                folderFilterMenu_->addAction(action);
        }
        folderFilterMenu_->addSeparator();
        folderFilterMenu_->addAction(doc->action(QStringLiteral("folder-ignore-case")));
        folderFilterMenu_->addAction(doc->action(QStringLiteral("folder-normalize-encoding")));
    }
}

MeldDoc *MeldWindow::currentDoc() const
{
    return qobject_cast<MeldDoc *>(tabs_->currentWidget());
}

QList<MeldDoc *> MeldWindow::docs() const
{
    QList<MeldDoc *> result;
    for (int i = 0; i < tabs_->count(); ++i) {
        if (auto *doc = qobject_cast<MeldDoc *>(tabs_->widget(i)))
            result.append(doc);
    }
    return result;
}

bool MeldWindow::hasPages() const
{
    return tabs_->count() > 0;
}

void MeldWindow::onCurrentTabChanged()
{
    MeldDoc *doc = currentDoc();
    if (filterConnection_)
        disconnect(filterConnection_);

    for (ProxyAction *action : std::as_const(proxies_))
        action->bind(doc ? doc->action(action->name()) : nullptr);
    boundDoc_ = doc;

    const ToolbarKind kind = doc ? doc->toolbarKind() : ToolbarKind::None;
    for (QAction *action : std::as_const(fileDiffToolbar_))
        action->setVisible(kind == ToolbarKind::FileDiff);
    for (QAction *action : std::as_const(dirDiffToolbar_))
        action->setVisible(kind == ToolbarKind::DirDiff);

    const FilterVisibility filters = doc ? doc->filterVisibility() : FilterVisibility{};
    textFilterButtonAction_->setVisible(filters.text);
    folderFilterButtonAction_->setVisible(filters.folder);
    vcFilterButtonAction_->setVisible(filters.vc);
    const bool conflicts = doc && doc->conflictVisibility();
    for (QAction *action : std::as_const(conflictActions_))
        action->setVisible(conflicts);

    windowActions_.value(QStringLiteral("win.close"))->setEnabled(doc != nullptr);

    if (doc) {
        filterConnection_ = connect(doc, &MeldDoc::filterActionsChanged, this, [this]() {
            updateFilterMenus();
            onCurrentTabChanged();
        });
        updateFilterMenus();
        doc->onContainerSwitchIn();
        scheduler_.addTask(doc->scheduler().asTask());
        setWindowTitle(doc->tabTitle().isEmpty() ? tr("Meld Qt") : tr("%1 - Meld Qt").arg(doc->tabTitle()));
    } else {
        textFilterMenu_->clear();
        folderFilterMenu_->clear();
        vcFilterMenu_->clear();
        setWindowTitle(tr("Meld Qt"));
    }
}

void MeldWindow::onTabLabelChanged(MeldDoc *doc)
{
    const int index = tabs_->indexOf(doc);
    if (index < 0)
        return;
    tabs_->setTabText(index, doc->tabTitle());
    tabs_->setTabToolTip(index, doc->tabTooltip());
    if (doc == currentDoc())
        setWindowTitle(doc->tabTitle().isEmpty() ? tr("Meld Qt") : tr("%1 - Meld Qt").arg(doc->tabTitle()));
}

void MeldWindow::appendPage(MeldDoc *doc)
{
    // Change focus to the newly created page only if the user is on a
    // DirDiff or VcView page, or if it's a new tab page. This prevents
    // cycling through X pages when X diffs are initiated.
    const bool select = qobject_cast<DirDiff *>(currentDoc()) || qobject_cast<VcView *>(currentDoc())
        || qobject_cast<NewDiffTab *>(doc) || !hasPages();

    const int index = tabs_->addTab(doc, doc->tabTitle());
    tabs_->setTabToolTip(index, doc->tabTooltip());
    connect(doc, &MeldDoc::tabLabelChanged, this, [this, doc]() { onTabLabelChanged(doc); });

    scheduler_.addScheduler(&doc->scheduler());
    connect(doc, &MeldDoc::fileChanged, this, [this, doc](const QString &path) {
        for (MeldDoc *other : docs()) {
            if (other != doc)
                other->onFileChanged(path);
        }
    });
    connect(doc, &MeldDoc::createDiff, this, [this](const QStringList &paths, const DiffOptions &options) {
        try {
            appendDiff(paths, options.autoCompare, options.autoMerge, options.mergeOutput, options.meta);
        } catch (const std::exception &e) {
            QMessageBox::warning(this, tr("Couldn't open comparison"), QString::fromStdString(e.what()));
        }
    });
    connect(doc, &MeldDoc::stateChanged, this, [this](MeldDoc::State oldState, MeldDoc::State) {
        if (shouldClose_ && oldState == MeldDoc::State::Closing) {
            // Cancel closing if one of our tabs does
            shouldClose_ = false;
        }
    });
    connect(doc, &MeldDoc::closed, this, [this, doc]() { pageRemoved(doc); });

    if (select)
        tabs_->setCurrentWidget(doc);
}

void MeldWindow::pageRemoved(MeldDoc *doc)
{
    scheduler_.removeScheduler(&doc->scheduler());
    const int index = tabs_->indexOf(doc);
    if (index >= 0)
        tabs_->removeTab(index);
    doc->hide();
    doc->deleteLater();

    const bool removedLastPage = !hasPages();
    if (removedLastPage)
        onCurrentTabChanged();

    if (shouldClose_) {
        if (removedLastPage)
            close();
        else
            QTimer::singleShot(0, this, &MeldWindow::closeWindowAsync);
    }
}

void MeldWindow::closeWindowAsync()
{
    // Delete pages from right-to-left. This ensures that if a version control
    // page is open in the far left page, it will be closed last.
    if (hasPages()) {
        tabs_->setCurrentIndex(tabs_->count() - 1);
        if (MeldDoc *doc = currentDoc())
            doc->requestClose();
    } else {
        close();
    }
}

void MeldWindow::closeEvent(QCloseEvent *event)
{
    if (hasPages()) {
        shouldClose_ = true;
        QTimer::singleShot(0, this, &MeldWindow::closeWindowAsync);
        // prevent close, will be done once all pages are closed
        event->ignore();
        return;
    }
    saveWindowState();
    event->accept();
}

void MeldWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls())
        event->acceptProposedAction();
}

void MeldWindow::dropEvent(QDropEvent *event)
{
    QStringList paths;
    for (const QUrl &url : event->mimeData()->urls()) {
        if (url.isLocalFile())
            paths.append(url.toLocalFile());
    }
    if (paths.isEmpty())
        return;
    QString error;
    openPaths(paths, false, false, true, &error);
    if (!error.isEmpty())
        qCWarning(lcWindow) << "Couldn't open comparison:" << error;
    event->acceptProposedAction();
}

void MeldWindow::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange) {
        // As the original on Adw.StyleManager:dark changes, this updates
        // the scheme and tells everything using it
        if (updateTheme())
            Q_EMIT MeldSettings::instance()->changed(QStringLiteral("style-scheme"));
        for (MeldDoc *doc : docs())
            doc->update();
    }
    QMainWindow::changeEvent(event);
}

void MeldWindow::onSchedulerRunnable()
{
    if (!idleTimer_.isActive()) {
        spinner_->start();
        windowActions_.value(QStringLiteral("win.stop"))->setEnabled(true);
        idleTimer_.start();
    }
}

void MeldWindow::onIdle()
{
    // Run tasks for a short time slice, so that the UI stays responsive
    QElapsedTimer timer;
    timer.start();
    while (scheduler_.tasksPending() && timer.elapsed() < 12) {
        const StepResult result = scheduler_.iteration();
        if (!result.status.isEmpty())
            spinner_->setToolTip(result.status);
    }

    if (!scheduler_.tasksPending()) {
        idleTimer_.stop();
        spinner_->stop();
        spinner_->setToolTip(QString());
        if (QAction *stop = windowActions_.value(QStringLiteral("win.stop")))
            stop->setEnabled(false);
    }
}

void MeldWindow::runPendingTasks()
{
    while (scheduler_.tasksPending())
        onIdle();
}

NewDiffTab *MeldWindow::appendNewComparison()
{
    auto *doc = new NewDiffTab(this);
    appendPage(doc);
    connect(doc, &NewDiffTab::diffRequested, this, [this, doc](NewDiffTab::DiffType type, const QStringList &paths) {
        MeldDoc *newdoc = nullptr;
        try {
            if (type == NewDiffTab::DiffType::File)
                newdoc = appendFilediff(paths);
            else if (type == NewDiffTab::DiffType::Folder)
                newdoc = appendDirdiff(paths);
            else if (type == NewDiffTab::DiffType::Version)
                newdoc = appendVcview(paths.value(0));
        } catch (const std::exception &e) {
            QMessageBox::warning(this, tr("Couldn't open comparison"), QString::fromStdString(e.what()));
            return;
        }
        if (!newdoc)
            return;
        if (const auto comparison = newdoc->comparison())
            RecentComparisons::add(*comparison);
        doc->requestClose();
        tabs_->setCurrentWidget(newdoc);
    });
    connect(doc, &NewDiffTab::patchDiffRequested, this,
            [this, doc](NewDiffTab::DiffType, const QString &path, const QString &patchFile) {
                MeldDoc *newdoc = nullptr;
                try {
                    newdoc = appendPatchDiff(path, patchFile);
                } catch (const std::exception &e) {
                    QMessageBox::warning(this, tr("Couldn't open comparison"), QString::fromStdString(e.what()));
                    return;
                }
                doc->requestClose();
                tabs_->setCurrentWidget(newdoc);
            });
    return doc;
}

DirDiff *MeldWindow::appendDirdiff(const QStringList &folders, bool autoCompare)
{
    auto *doc = new DirDiff(int(folders.size()), this);
    appendPage(doc);
    doc->setFolders(folders);
    doc->setLocations();
    if (autoCompare) {
        QPointer<DirDiff> guard(doc);
        doc->scheduler().addOneShot([guard]() {
            if (guard)
                guard->autoCompare();
        });
    }
    return doc;
}

FileDiff *MeldWindow::appendFilediff(const QStringList &files, const QList<QByteArray> &encodings,
                                     const QString &mergeOutput, const std::optional<ComparisonMeta> &meta)
{
    auto *doc = new FileDiff(int(files.size()), FileComparisonMode::Compare, this);
    appendPage(doc);
    if (meta)
        doc->setVirtualFiles(meta->virtualFiles);
    doc->setFiles(files, encodings);
    if (!mergeOutput.isEmpty())
        doc->setMergeOutputFile(mergeOutput);
    if (meta)
        doc->setMeta(*meta);
    return doc;
}

FileDiff *MeldWindow::appendFilemerge(const QStringList &files, const QString &mergeOutput)
{
    if (files.size() != 3)
        throw std::invalid_argument(tr("Need three files to auto-merge, got: %1").arg(files.join(QStringLiteral(", "))).toStdString());
    auto *doc = new FileDiff(3, FileComparisonMode::AutoMerge, this);
    appendPage(doc);
    doc->setFiles(files);
    if (!mergeOutput.isEmpty())
        doc->setMergeOutputFile(mergeOutput);
    return doc;
}

MeldDoc *MeldWindow::appendDiff(const QStringList &paths, bool autoCompare, bool autoMerge, const QString &mergeOutput,
                                const std::optional<ComparisonMeta> &meta)
{
    bool haveDirectories = false;
    bool haveFiles = false;
    for (const QString &path : paths) {
        if (path.isEmpty())
            continue;
        if (QFileInfo(path).isDir())
            haveDirectories = true;
        else
            haveFiles = true;
    }
    if (haveDirectories && haveFiles)
        throw std::invalid_argument(tr("Cannot compare a mixture of files and directories").toStdString());
    if (haveDirectories)
        return appendDirdiff(paths, autoCompare);
    if (autoMerge)
        return appendFilemerge(paths, mergeOutput);
    return appendFilediff(paths, {}, mergeOutput, meta);
}

MeldDoc *MeldWindow::appendPatchDiff(const QString &path, const QString &patchFile)
{
    if (path.isEmpty() || patchFile.isEmpty())
        throw std::invalid_argument(tr("Select what to compare, and the patch to apply to it.").toStdString());
    const QString patchName = QDir::toNativeSeparators(patchFile);
    QString error;
    if (QFileInfo(path).isDir()) {
        std::shared_ptr<patch::PatchedFolder> patched = patch::PatchedFolder::load(path, patchFile, &error);
        if (!patched)
            throw std::invalid_argument(tr("The patch “%1” can’t be read.\n\n%2").arg(patchName, error).toStdString());
        auto *doc = new DirDiff(2, this);
        appendPage(doc);
        doc->setPatch(patched);
        doc->setFolders({path, patchFile});
        doc->setLocations();
        return doc;
    }

    // The patch is checked here, so that one for other files doesn't open a
    // comparison; it's applied as the comparison loads
    const std::optional<patch::Patch> patch = patch::parseFile(patchFile, &error);
    if (!patch)
        throw std::invalid_argument(tr("The patch “%1” can’t be read.\n\n%2").arg(patchName, error).toStdString());
    if (!patch::findFilePatch(*patch, path, &error))
        throw std::invalid_argument(error.toStdString());
    ComparisonMeta meta;
    meta.virtualFiles.insert(1, patchedFile(path, patchFile));
    meta.tabLabel = patchComparisonTabLabel(path, patchFile);
    return appendFilediff({path, QString()}, {}, QString(), meta);
}

VcView *MeldWindow::appendVcview(const QString &location, bool autoCompare)
{
    auto *doc = new VcView(this);
    appendPage(doc);
    if (!location.isEmpty())
        doc->setLocation(location);
    if (autoCompare) {
        QPointer<VcView> guard(doc);
        doc->scheduler().addOneShot([guard]() {
            if (guard)
                guard->autoCompare();
        });
    }
    return doc;
}

MeldDoc *MeldWindow::appendRecent(const Comparison &comparison)
{
    // Folder comparisons of archives are recorded with the archives, which
    // are extracted again
    QStringList paths = comparison.paths;
    if (comparison.type == RecentType::Folder || comparison.type == RecentType::VersionControl) {
        QString error;
        if (!extractArchives(paths, &error))
            return nullptr;
    }

    MeldDoc *tab = nullptr;
    switch (comparison.type) {
    case RecentType::File:
        tab = appendFilediff(paths);
        break;
    case RecentType::Folder:
        tab = appendDirdiff(paths);
        break;
    case RecentType::Merge:
        tab = appendFilemerge(paths);
        break;
    case RecentType::VersionControl:
        tab = appendVcview(paths.value(0));
        break;
    }
    tabs_->setCurrentWidget(tab);
    RecentComparisons::add(comparison);
    return tab;
}

bool MeldWindow::extractArchives(QStringList &paths, QString *error)
{
    for (QString &path : paths) {
        if (!archive::isArchive(path))
            continue;

        // Archives are extracted one at a time, which keeps error handling
        // simple. Extracting takes a while for large archives, so it can be
        // cancelled.
        archive::Extraction extraction(path);
        QProgressDialog progress(tr("Opening archive “%1”…").arg(QFileInfo(path).fileName()), tr("Cancel"), 0, 0,
                                 this);
        progress.setWindowTitle(tr("Opening Archive"));
        progress.setWindowModality(Qt::WindowModal);
        progress.setMinimumDuration(500);
        progress.setAutoClose(false);
        progress.setAutoReset(false);

        QEventLoop loop;
        bool done = false;
        QString root;
        QString failure;
        connect(&extraction, &archive::Extraction::finished, &loop, [&](const QString &extracted) {
            root = extracted;
            done = true;
            loop.quit();
        });
        connect(&extraction, &archive::Extraction::failed, &loop, [&](const QString &message) {
            failure = message;
            done = true;
            loop.quit();
        });
        connect(&progress, &QProgressDialog::canceled, &loop, [&]() {
            extraction.cancel();
            done = true;
            loop.quit();
        });
        extraction.start();
        if (!done) {
            progress.setValue(0);
            loop.exec();
        }
        progress.hide();

        if (root.isEmpty() && failure.isEmpty()) {
            if (error)
                error->clear();
            return false;
        }
        if (!failure.isEmpty()) {
            const QString message = tr("Couldn’t open archive %1: %2").arg(path, failure);
            if (error)
                *error = message;
            // On the command line, the error is printed; this is for
            // archives opened in a shown window
            if (isVisible()) {
                auto *dialog = new QMessageBox(QMessageBox::Warning, tr("Failed to open archive"), message,
                                               QMessageBox::Close, this);
                dialog->setAttribute(Qt::WA_DeleteOnClose);
                dialog->setWindowModality(Qt::WindowModal);
                dialog->show();
            }
            return false;
        }
        path = root;
    }
    return true;
}

MeldDoc *MeldWindow::openPaths(const QStringList &givenPaths, bool autoCompare, bool autoMerge, bool focus,
                               QString *error)
{
    QStringList paths = givenPaths;
    if (!paths.isEmpty() && paths.size() <= 3 && !extractArchives(paths, error))
        return nullptr;

    MeldDoc *tab = nullptr;
    try {
        if (paths.size() == 1) {
            const QString path = paths.first();
            if (!path.isEmpty() && !QFileInfo::exists(path)) {
                if (error)
                    *error = tr("Cannot compare a non-existent file");
                return nullptr;
            }
            if (path.isEmpty() || QFileInfo(path).isDir()) {
                tab = appendVcview(path, autoCompare);
            } else {
                // As in the original, this isn't a recent comparison, and
                // isn't given labels or an output file
                singleFileOpen(path);
                return nullptr;
            }
        } else if (paths.size() == 2 || paths.size() == 3) {
            tab = appendDiff(paths, autoCompare, autoMerge);
        } else if (paths.size() > 3) {
            if (error)
                *error = tr("too many arguments (wanted 0-3, got %1)").arg(paths.size());
            return nullptr;
        }
    } catch (const std::exception &e) {
        if (error)
            *error = QString::fromStdString(e.what());
        return nullptr;
    }

    if (tab) {
        if (const auto comparison = tab->comparison())
            RecentComparisons::add(*comparison);
        if (focus)
            tabs_->setCurrentWidget(tab);
    }
    return tab;
}

MeldDoc *MeldWindow::openPatch(const QString &givenPath, const QString &patchFile, bool autoCompare, bool focus,
                               QString *error)
{
    if (givenPath.isEmpty() || !QFileInfo::exists(givenPath)) {
        if (error)
            *error = tr("Cannot compare a non-existent file");
        return nullptr;
    }
    if (!QFileInfo(patchFile).isFile()) {
        if (error)
            *error = tr("The patch “%1” doesn’t exist").arg(QDir::toNativeSeparators(patchFile));
        return nullptr;
    }
    QStringList paths{givenPath};
    if (!extractArchives(paths, error))
        return nullptr;

    MeldDoc *tab = nullptr;
    try {
        tab = appendPatchDiff(paths.first(), patchFile);
    } catch (const std::exception &e) {
        if (error)
            *error = QString::fromStdString(e.what());
        if (archive::isExtractedArchiveRoot(paths.first()))
            archive::removeExtractedArchives({paths.first()});
        return nullptr;
    }
    if (auto *dirdiff = qobject_cast<DirDiff *>(tab); dirdiff && autoCompare) {
        QPointer<DirDiff> guard(dirdiff);
        dirdiff->scheduler().addOneShot([guard]() {
            if (guard)
                guard->autoCompare();
        });
    }
    if (focus)
        tabs_->setCurrentWidget(tab);
    return tab;
}

MeldDoc *MeldWindow::singleFileOpen(const QString &path)
{
    const VcChoices choices = vcChoicesForLocation(path);
    const std::shared_ptr<vc::Vc> vc = choices.choices.at(size_t(choices.defaultIndex)).vc;
    // Ensure that we have the correct state for the file we're opening
    vc->refreshVcState();

    // A file with conflict markers is merged, unless version control has
    // the sides of the conflict, which are whole files
    const QString file = vc::abspath(path);
    const bool vcConflict = vc->entry(file).state == StateConflict && vc->hasConflictSupport();
    if (!vcConflict && conflictsInFile(file) > 0) {
        const bool versioned = choices.choices.at(size_t(choices.defaultIndex)).enabled;
        const DiffOptions options = conflictResolution(file, versioned ? vcResolver(vc) : nullptr);
        return appendDiff({QString(), QString(), QString()}, false, false, options.mergeOutput, options.meta);
    }

    const VcFileComparison comparison = vcFileComparison(vc, file);
    return appendDiff(comparison.paths, false, comparison.options.autoMerge, comparison.options.mergeOutput,
                      comparison.options.meta);
}

void MeldWindow::showKeyboardShortcuts()
{
    // The sections of Meld's shortcuts window (help-overlay.ui); entries
    // either name an action, whose shortcuts are shown, or give a shortcut
    struct Entry {
        const char *title;
        const char *action;
        const char *shortcut;
    };
    struct Section {
        const char *title;
        std::vector<Entry> entries;
    };
    static const std::vector<Section> sections{
        {QT_TR_NOOP("Windows and Tabs"),
         {
             {QT_TR_NOOP("New comparison"), "win.new-tab", nullptr},
             {QT_TR_NOOP("Close a comparison"), "win.close", nullptr},
             {QT_TR_NOOP("Quit Meld Qt"), "app.quit", nullptr},
         }},
        {QT_TR_NOOP("General"),
         {
             {QT_TR_NOOP("Show preferences"), "app.preferences", nullptr},
             {QT_TR_NOOP("Keyboard shortcuts"), "win.show-help-overlay", nullptr},
             {QT_TR_NOOP("Open menu"), "win.gear-menu", nullptr},
             {QT_TR_NOOP("Fullscreen"), "win.fullscreen", nullptr},
         }},
        {QT_TR_NOOP("Common Actions"),
         {
             {QT_TR_NOOP("Stop the current action"), "win.stop", nullptr},
             {QT_TR_NOOP("Refresh comparison"), "view.refresh", nullptr},
             {QT_TR_NOOP("Open externally"), "view.open-external", nullptr},
         }},
        {QT_TR_NOOP("Panes"),
         {
             {QT_TR_NOOP("Previous comparison pane"), "view.previous-pane", nullptr},
             {QT_TR_NOOP("Next comparison pane"), "view.next-pane", nullptr},
             {QT_TR_NOOP("Swap left and right panes"), "view.swap-2-panes", nullptr},
         }},
        {QT_TR_NOOP("Changes"),
         {
             {QT_TR_NOOP("Go to previous change"), nullptr, "Alt+Up"},
             {QT_TR_NOOP("Go to next change"), nullptr, "Alt+Down"},
         }},
        {QT_TR_NOOP("Tabs"),
         {
             {QT_TR_NOOP("Go to previous tab"), "win.previous-tab", nullptr},
             {QT_TR_NOOP("Go to next tab"), "win.next-tab", nullptr},
             {QT_TR_NOOP("Switch to tab"), nullptr, "Alt+0...9"},
             {QT_TR_NOOP("Move tab left"), "win.move-tab-left", nullptr},
             {QT_TR_NOOP("Move tab right"), "win.move-tab-right", nullptr},
         }},
        {QT_TR_NOOP("Editing"),
         {
             {QT_TR_NOOP("Undo"), "view.undo", nullptr},
             {QT_TR_NOOP("Redo"), "view.redo", nullptr},
             {QT_TR_NOOP("Cut"), nullptr, "Ctrl+X"},
             {QT_TR_NOOP("Copy"), nullptr, "Ctrl+C"},
             {QT_TR_NOOP("Paste"), nullptr, "Ctrl+V"},
             {QT_TR_NOOP("Find"), "view.find", nullptr},
             {QT_TR_NOOP("Find Next"), nullptr, "Ctrl+G"},
             {QT_TR_NOOP("Find Previous"), nullptr, "Ctrl+Shift+G"},
             {QT_TR_NOOP("Replace"), "view.find-replace", nullptr},
             {QT_TR_NOOP("Go to line"), "view.go-to-line", nullptr},
         }},
        {QT_TR_NOOP("File comparison"),
         {
             {QT_TR_NOOP("Save current file"), "view.save", nullptr},
             {QT_TR_NOOP("Save current file to new path"), "view.save-as", nullptr},
             {QT_TR_NOOP("Save all files in comparison"), "view.save-all", nullptr},
             {QT_TR_NOOP("Previous conflict"), "view.file-previous-conflict", nullptr},
             {QT_TR_NOOP("Next conflict"), "view.file-next-conflict", nullptr},
             {QT_TR_NOOP("Push change to left"), "view.file-push-left", nullptr},
             {QT_TR_NOOP("Push change to right"), "view.file-push-right", nullptr},
             {QT_TR_NOOP("Pull change from left"), "view.file-pull-left", nullptr},
             {QT_TR_NOOP("Pull change from right"), "view.file-pull-right", nullptr},
             {QT_TR_NOOP("Copy change above left"), "view.file-copy-left-up", nullptr},
             {QT_TR_NOOP("Copy change below left"), "view.file-copy-left-down", nullptr},
             {QT_TR_NOOP("Copy change above right"), "view.file-copy-right-up", nullptr},
             {QT_TR_NOOP("Copy change below right"), "view.file-copy-right-down", nullptr},
             {QT_TR_NOOP("Delete change"), nullptr, "Alt+Del"},
             {QT_TR_NOOP("Show overview map"), "view.show-overview-map", nullptr},
             {QT_TR_NOOP("Zoom in"), "view.zoom-in", nullptr},
             {QT_TR_NOOP("Zoom out"), "view.zoom-out", nullptr},
             {QT_TR_NOOP("Reset zoom"), "view.zoom-reset", nullptr},
         }},
        {QT_TR_NOOP("Folder comparison"),
         {
             {QT_TR_NOOP("Copy to left"), "view.folder-copy-left", nullptr},
             {QT_TR_NOOP("Copy to right"), "view.folder-copy-right", nullptr},
             {QT_TR_NOOP("Delete change"), nullptr, "Del"},
             {QT_TR_NOOP("Expand the current folder"), nullptr, "+"},
             {QT_TR_NOOP("Collapse the current folder"), nullptr, "-"},
         }},
        {QT_TR_NOOP("Version control comparison"),
         {
             {QT_TR_NOOP("Commit to version control"), "view.vc-commit", nullptr},
             {QT_TR_NOOP("Show/hide console output"), "view.vc-console-visible", nullptr},
         }},
    };

    const auto nativeText = [](const QString &portable) {
        return QKeySequence::fromString(portable, QKeySequence::PortableText).toString(QKeySequence::NativeText);
    };

    auto *dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(tr("Keyboard Shortcuts"));
    dialog->resize(560, 640);
    auto *layout = new QVBoxLayout(dialog);
    auto *tree = new QTreeWidget(dialog);
    tree->setColumnCount(2);
    tree->setHeaderHidden(true);
    tree->setRootIsDecorated(false);
    tree->setItemsExpandable(false);
    tree->setSelectionMode(QAbstractItemView::NoSelection);
    tree->setFocusPolicy(Qt::NoFocus);
    tree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    QFont sectionFont = tree->font();
    sectionFont.setBold(true);

    for (const Section &section : sections) {
        auto *sectionItem = new QTreeWidgetItem(tree, {tr(section.title)});
        sectionItem->setFont(0, sectionFont);
        sectionItem->setFirstColumnSpanned(true);
        for (const Entry &entry : section.entries) {
            QStringList keys;
            if (entry.action) {
                QList<QKeySequence> sequences = shortcutsForAction(QLatin1String(entry.action));
                if (QAction *action = windowActions_.value(QLatin1String(entry.action)); sequences.isEmpty() && action)
                    sequences = action->shortcuts();
                for (const QKeySequence &sequence : std::as_const(sequences))
                    keys.append(sequence.toString(QKeySequence::NativeText));
            } else if (QLatin1String(entry.shortcut) == QLatin1String("Alt+0...9")) {
                keys.append(nativeText(QStringLiteral("Alt+0")) + QStringLiteral("…9"));
            } else {
                keys.append(nativeText(QLatin1String(entry.shortcut)));
            }
            new QTreeWidgetItem(sectionItem, {keys.join(QStringLiteral(" / ")), tr(entry.title)});
        }
    }
    tree->expandAll();
    layout->addWidget(tree);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog->open();
}

} // namespace meld
