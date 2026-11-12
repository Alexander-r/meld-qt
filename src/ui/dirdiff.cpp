#include "dirdiff.h"

#include "chunkmap.h"
#include "core/archive.h"
#include "core/patch.h"
#include "core/iohelpers.h"
#include "core/misc.h"
#include "externalhelpers.h"
#include "icons.h"
#include "linkmap.h"
#include "msgarea.h"
#include "patchcomparison.h"
#include "pathlabel.h"
#include "settings.h"
#include "trash.h"

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QScrollBar>
#include <QVBoxLayout>

#include <algorithm>

namespace meld {

namespace {

QString folderFilterActionName(int index)
{
    return QStringLiteral("folder-custom-filter-%1").arg(index);
}

QString joinPath(const QString &root, const QString &name)
{
    if (root.isEmpty())
        return name;
    if (root.endsWith(QLatin1Char('/')))
        return root + name;
    return root + QLatin1Char('/') + name;
}

bool isDir(const QString &path)
{
    return !path.isEmpty() && QFileInfo(path).isDir();
}

bool exists(const QString &path)
{
    return !path.isEmpty() && QFileInfo::exists(path);
}

int unixPermissions(QFile::Permissions permissions)
{
    int mode = 0;
    const std::pair<QFile::Permission, int> bits[] = {
        {QFile::ReadOwner, 0400}, {QFile::WriteOwner, 0200}, {QFile::ExeOwner, 0100},
        {QFile::ReadGroup, 040},  {QFile::WriteGroup, 020},  {QFile::ExeGroup, 010},
        {QFile::ReadOther, 04},   {QFile::WriteOther, 02},   {QFile::ExeOther, 01},
    };
    for (const auto &[permission, bit] : bits) {
        if (permissions & permission)
            mode |= bit;
    }
    return mode;
}

//! Like shutil.copy2, but ignores chmod errors, and copies symlinks as links
bool copyFile(const QString &src, const QString &dstDir, QString *error)
{
    const QFileInfo info(src);
    const QString dst = joinPath(dstDir, info.fileName());
    if (info.isSymLink() && info.isFile()) {
        QFile::remove(dst);
        if (!QFile::link(info.symLinkTarget(), dst)) {
            *error = QObject::tr("Couldn't create link %1").arg(dst);
            return false;
        }
        return true;
    }
    if (!info.isFile()) {
        *error = QObject::tr("Not a file");
        return false;
    }
    if (QFileInfo::exists(dst) && !QFile::remove(dst)) {
        *error = QObject::tr("Couldn't replace %1").arg(dst);
        return false;
    }
    QFile source(src);
    if (!source.copy(dst)) {
        *error = source.errorString();
        return false;
    }
    QFile target(dst);
    if (target.open(QIODevice::ReadWrite)) {
        target.setFileTime(info.lastModified(), QFileDevice::FileModificationTime);
        target.close();
    }
    return true;
}

//! Similar to shutil.copytree, but always copies symlinks and doesn't error
//! out if the destination path already exists.
bool copyTree(const QString &src, const QString &dst, QString *error)
{
    const QFileInfo info(src);
    if (info.isSymLink())
        return QFile::link(info.symLinkTarget(), dst);
    if (!QDir().mkpath(dst)) {
        *error = QObject::tr("Couldn't create folder %1").arg(dst);
        return false;
    }
    const QDir dir(src);
    for (const QFileInfo &entry : dir.entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System
                                                    | QDir::NoDotAndDotDot)) {
        const QString target = joinPath(dst, entry.fileName());
        if (entry.isSymLink()) {
            QFile::remove(target);
            QFile::link(entry.symLinkTarget(), target);
        } else if (entry.isDir()) {
            if (!copyTree(entry.absoluteFilePath(), target, error))
                return false;
        } else if (!copyFile(entry.absoluteFilePath(), dst, error)) {
            return false;
        }
    }
    return true;
}

class BorderedBar : public QWidget {
public:
    using QWidget::QWidget;

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setPen(palette().color(QPalette::Mid));
        painter.drawLine(0, height() - 1, width(), height() - 1);
    }
};

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

void expandToIndex(QTreeView *view, const QModelIndex &index)
{
    std::vector<QModelIndex> chain;
    for (QModelIndex i = index; i.isValid(); i = i.parent())
        chain.push_back(i);
    for (auto it = chain.rbegin(); it != chain.rend(); ++it)
        view->expand(*it);
}

} // namespace

// ---------------------------------------------------------------------------
// CanonicalListing

CanonicalListing::CanonicalListing(int n, ComparisonOptions options)
    : n_(n)
    , options_(options)
{
}

void CanonicalListing::add(int pane, const QString &item)
{
    // normalize the name depending on settings
    QString ci = item;
    if (options_.ignoreCase)
        ci = ci.toLower();
    if (options_.normalizeEncoding) {
        // NFC or NFD will work here, changing all composed or decomposed
        // characters to the same set for matching only.
        ci = ci.normalized(QString::NormalizationForm_C);
    }

    // add the item to the comparison tree
    QStringList &entry = items_[ci];
    if (entry.isEmpty()) {
        for (int i = 0; i < n_; ++i)
            entry.append(QString());
    }
    const QString existing = entry.at(pane);
    if (existing.isEmpty())
        entry[pane] = item;
    else
        errors.push_back({pane, item, existing});

    const QString stripped = ci.trimmed();
    if (auto it = strippedItems_.constFind(stripped); it != strippedItems_.constEnd()) {
        // If we have an existing stripped item and its pre-stripping value
        // differs, then we have a case of misleading whitespace
        if (it.value() != ci)
            whitespace.emplace_back(pane, item);
    } else {
        strippedItems_.insert(stripped, ci);
    }
}

std::vector<QStringList> CanonicalListing::get() const
{
    std::vector<QStringList> result;
    for (const auto &[key, values] : items_) {
        QString fill;
        for (const QString &v : values) {
            if (!v.isEmpty()) {
                fill = v;
                break;
            }
        }
        QStringList filled;
        for (const QString &v : values)
            filled.append(v.isEmpty() ? fill : v);
        result.push_back(filled);
    }
    std::sort(result.begin(), result.end());
    return result;
}

// ---------------------------------------------------------------------------
// DirTreeView

DirTreeView::DirTreeView(QWidget *parent)
    : QTreeView(parent)
{
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setSelectionBehavior(QAbstractItemView::SelectRows);
    setUniformRowHeights(true);
    setExpandsOnDoubleClick(false);
    setAllColumnsShowFocus(true);
    setFrameShape(QFrame::NoFrame);
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    header()->setStretchLastSection(false);
    header()->setSectionsMovable(false);
}

bool DirTreeView::searchMatches(const QStringList &paths, const QString &key)
{
    // If the key contains a path separator, search the whole path, otherwise
    // just use the filename. If the key is all lower-case, do a
    // case-insensitive match.
    const bool absSearch = key.contains(QLatin1Char('/'));
    const bool lowerKey = key == key.toLower();
    for (const QString &path : paths) {
        if (path.isEmpty())
            continue;
        QString text = absSearch ? path : path::basename(path);
        if (lowerKey)
            text = text.toLower();
        if (text.contains(key))
            return true;
    }
    return false;
}

void DirTreeView::focusInEvent(QFocusEvent *event)
{
    QTreeView::focusInEvent(event);
    Q_EMIT focusedIn();
}

void DirTreeView::keyPressEvent(QKeyEvent *event)
{
    if (event->modifiers() == Qt::NoModifier || event->modifiers() == Qt::KeypadModifier) {
        if (event->key() == Qt::Key_Left || event->key() == Qt::Key_Right) {
            Q_EMIT paneSwitchRequested(event->key() == Qt::Key_Right);
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
            Q_EMIT compareRequested();
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Delete) {
            Q_EMIT deleteRequested();
            event->accept();
            return;
        }
    }
    QTreeView::keyPressEvent(event);
}

void DirTreeView::contextMenuEvent(QContextMenuEvent *event)
{
    setFocus(Qt::MouseFocusReason);
    const QModelIndex index = indexAt(event->pos());
    if (!index.isValid())
        return;
    if (!selectionModel()->isSelected(index)) {
        selectionModel()->clearSelection();
        setCurrentIndex(index);
        selectionModel()->select(index, QItemSelectionModel::Select | QItemSelectionModel::Rows);
    }
    Q_EMIT contextMenuRequested(event->globalPos());
    event->accept();
}

// ---------------------------------------------------------------------------
// DirDiff construction

DirDiff::DirDiff(int numPanes, QWidget *parent)
    : MeldDoc(parent)
{
    MeldSettings *settings = MeldSettings::instance();
    ignoreSymlinks_ = settings->boolValue(QStringLiteral("folder-ignore-symlinks"));
    showOverviewMap_ = settings->boolValue(QStringLiteral("show-overview-map"));

    buildLayout();
    createActions();
    createNameFilters();
    createTextFilters();

    // The list copying here is because the action toggled callback modifies
    // the state while we're constructing it.
    const QStringList statusFilters = settings->stringListValue(QStringLiteral("folder-status-filters"));
    const std::pair<int, std::pair<const char *, const char *>> stateActions[] = {
        {StateNormal, {"normal", "folder-status-same"}},
        {StateNoChange, {"normal", "folder-status-same"}},
        {StateNew, {"new", "folder-status-new"}},
        {StateModified, {"modified", "folder-status-modified"}},
    };
    for (const auto &[state, names] : stateActions) {
        if (statusFilters.contains(QLatin1String(names.first))) {
            stateFilters_.push_back(state);
            QAction *a = action(QLatin1String(names.second));
            const QSignalBlocker blocker(a);
            a->setChecked(true);
        }
    }

    updateComparator();
    setNumPanes(numPanes);
    updateTreeviewColumns();

    connect(settings, &MeldSettings::fileFiltersChanged, this, [this]() {
        if (createNameFilters())
            refresh();
        Q_EMIT filterActionsChanged();
    });
    connect(settings, &MeldSettings::textFiltersChanged, this, [this]() {
        if (createTextFilters())
            refresh();
    });
    connect(settings, &MeldSettings::changed, this, [this](const QString &key) {
        if (key == QLatin1String("folder-columns")) {
            updateTreeviewColumns();
        } else if (key == QLatin1String("folder-shallow-comparison") || key == QLatin1String("folder-time-resolution")
                   || key == QLatin1String("folder-filter-text") || key == QLatin1String("ignore-blank-lines")) {
            updateComparator();
            refresh();
        } else if (key == QLatin1String("folder-ignore-symlinks")) {
            ignoreSymlinks_ = MeldSettings::instance()->boolValue(key);
        } else if (key == QLatin1String("show-overview-map")) {
            action(QStringLiteral("show-overview-map"))->setChecked(MeldSettings::instance()->boolValue(key));
        }
    });
}

DirDiff::~DirDiff() = default;

void DirDiff::buildLayout()
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    auto *grid = new QGridLayout;
    grid_ = grid;
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(0);
    outer->addLayout(grid);

    static constexpr int paneColumns[3] = {0, 2, 4};
    for (int pane = 0; pane < 3; ++pane) {
        auto *bar = new BorderedBar(this);
        auto *barLayout = new QHBoxLayout(bar);
        barLayout->setContentsMargins(6, 4, 6, 5);
        barLayout->setSpacing(0);
        auto *open = new FileButton(pane, FileButton::Mode::SelectFolder, tr("Select Folder"), bar);
        open->setToolTip(tr("Select folder to open in this pane"));
        connect(open, &FileButton::fileSelected, this, [this](int p, const QString &folder) {
            while (folders_.size() <= p)
                folders_.append(QString());
            folders_[p] = folder;
            // In patch comparisons, the patch is for the first folder; a
            // folder instead of the patched one makes this a folder
            // comparison
            if (patched_ && p == 0)
                reloadPatch(folder);
            else if (patched_)
                patched_.reset();
            setLocations();
        });
        folderOpenButton_[size_t(pane)] = open;
        auto *label = new PathLabel(bar);
        label->setEmptyLabel(tr("Folder %1").arg(pane + 1));
        folderLabel_[size_t(pane)] = label;
        barLayout->addStretch(1);
        barLayout->addWidget(open);
        barLayout->addWidget(label, 100);
        barLayout->addStretch(1);
        paneActionbar_[size_t(pane)] = bar;
        grid->addWidget(bar, 0, paneColumns[pane]);

        auto *vbox = new QWidget(this);
        auto *vboxLayout = new QVBoxLayout(vbox);
        vboxLayout->setContentsMargins(0, 0, 0, 0);
        vboxLayout->setSpacing(0);
        msgarea_[size_t(pane)] = new MsgAreaController(vbox);
        vboxLayout->addWidget(msgarea_[size_t(pane)]);
        auto *view = new DirTreeView(vbox);
        treeview_[size_t(pane)] = view;
        vboxLayout->addWidget(view, 1);
        vbox_[size_t(pane)] = vbox;
        grid->addWidget(vbox, 1, paneColumns[pane]);

        connect(view, &DirTreeView::focusedIn, this, [this, pane]() { onFocusIn(pane); });
        connect(view, &DirTreeView::paneSwitchRequested, this, [this, pane](bool right) {
            const int target = right ? pane + 1 : pane - 1;
            if (target >= 0 && target < numPanes_)
                changeFocusedTree(pane, target);
        });
        connect(view, &DirTreeView::contextMenuRequested, this,
                [this, pane](const QPoint &pos) { showContextMenu(pane, pos); });
        connect(view, &DirTreeView::compareRequested, this, &DirDiff::actionDiff);
        connect(view, &DirTreeView::deleteRequested, this, [this]() {
            if (action(QStringLiteral("folder-delete"))->isEnabled())
                deleteSelected();
        });
        connect(view, &QTreeView::activated, this, [this, pane](const QModelIndex &index) { onRowActivated(pane, index); });
        connect(view, &QTreeView::expanded, this, [this, pane](const QModelIndex &index) { onRowExpanded(pane, index); });
        connect(view, &QTreeView::collapsed, this,
                [this, pane](const QModelIndex &index) { onRowCollapsed(pane, index); });
        connect(view->verticalScrollBar(), &QScrollBar::valueChanged, this, [this, pane]() { syncVScroll(pane); });
        connect(view->horizontalScrollBar(), &QScrollBar::valueChanged, this, [this, pane]() { syncHScroll(pane); });
    }

    for (int i = 0; i < 2; ++i) {
        dummyToolbarLinkmap_[size_t(i)] = new BorderedBar(this);
        grid->addWidget(dummyToolbarLinkmap_[size_t(i)], 0, 1 + i * 2);
        auto *linkmap = new ScrollLinkMap(this);
        linkmap_[size_t(i)] = linkmap;
        grid->addWidget(linkmap, 1, 1 + i * 2);
        connect(linkmap, &ScrollLinkMap::scrollRequested, this, &DirDiff::nextDiff);
    }

    chunkmapBox_ = new OverviewBox(this);
    auto *chunkLayout = new QHBoxLayout(chunkmapBox_);
    chunkLayout->setContentsMargins(3, 0, 2, 0);
    chunkLayout->setSpacing(2);
    for (int pane = 0; pane < 3; ++pane) {
        auto *chunkmap = new TreeViewChunkMap(
            treeview_[size_t(pane)],
            [this, pane](const QModelIndex &index) -> std::optional<ChunkTag> {
                if (!model_ || pane >= model_->ntree())
                    return std::nullopt;
                switch (model_->state(model_->node(index), pane)) {
                case StateNew:
                    return ChunkTag::Insert;
                case StateError:
                    return ChunkTag::Equal;
                case StateModified:
                    return ChunkTag::Replace;
                default:
                    return std::nullopt;
                }
            },
            chunkmapBox_);
        chunkmap_[size_t(pane)] = chunkmap;
        chunkLayout->addWidget(chunkmap);
    }
    grid->addWidget(chunkmapBox_, 1, 5);
    overviewToolbarSpacer_ = new BorderedBar(this);
    grid->addWidget(overviewToolbarSpacer_, 0, 5);

    searchEntry_ = new QLineEdit(this);
    searchEntry_->setPlaceholderText(tr("Search"));
    searchEntry_->setClearButtonEnabled(true);
    searchEntry_->hide();
    grid->addWidget(searchEntry_, 2, 0, 1, 6);
    searchEntry_->installEventFilter(this);
    connect(searchEntry_, &QLineEdit::textChanged, this, [this](const QString &text) {
        if (text.isEmpty() || !model_)
            return;
        const int pane = std::max(0, focusedPane());
        DirTreeView *view = treeview_[size_t(pane)];
        TreeNode *start = view->currentIndex().isValid() ? model_->node(view->currentIndex()) : model_->firstRoot();
        for (TreeNode *n = start; n; n = model_->inorderNext(n)) {
            if (DirTreeView::searchMatches(model_->valuePaths(n), text)) {
                const QModelIndex index = model_->indexOf(n);
                expandToIndex(view, index.parent());
                view->setCurrentIndex(index);
                view->scrollTo(index);
                return;
            }
        }
    });
    connect(searchEntry_, &QLineEdit::returnPressed, this, [this]() {
        if (!model_)
            return;
        const int pane = std::max(0, focusedPane());
        DirTreeView *view = treeview_[size_t(pane)];
        const QString text = searchEntry_->text();
        TreeNode *start = view->currentIndex().isValid() ? model_->node(view->currentIndex()) : model_->firstRoot();
        for (TreeNode *n = model_->inorderNext(start); n; n = model_->inorderNext(n)) {
            if (DirTreeView::searchMatches(model_->valuePaths(n), text)) {
                const QModelIndex index = model_->indexOf(n);
                expandToIndex(view, index.parent());
                view->setCurrentIndex(index);
                view->scrollTo(index);
                return;
            }
        }
        errorBell();
    });
}

void DirDiff::createActions()
{
    addToggleAction(QStringLiteral("show-overview-map"), showOverviewMap_, [this](bool show) {
        showOverviewMap_ = show;
        chunkmapBox_->setVisible(show);
        overviewToolbarSpacer_->setVisible(show);
    });
    chunkmapBox_->setVisible(showOverviewMap_);
    overviewToolbarSpacer_->setVisible(showOverviewMap_);

    addAction(QStringLiteral("find"), [this]() { startSearch(); });
    addAction(QStringLiteral("folder-collapse"), [this]() { actionCollapse(); });
    addAction(QStringLiteral("folder-compare"), [this]() { actionDiff(); });
    addAction(QStringLiteral("folder-mark"), [this]() { actionMark(); });
    addAction(QStringLiteral("folder-compare-marked"), [this]() { actionDiffMarked(); });
    addAction(QStringLiteral("folder-copy-left"), [this]() { copySelected(-1); });
    addAction(QStringLiteral("folder-copy-right"), [this]() { copySelected(1); });
    addAction(QStringLiteral("swap-2-panes"), [this]() {
        std::reverse(folders_.begin(), folders_.end());
        refresh();
    });
    addAction(QStringLiteral("folder-delete"), [this]() { deleteSelected(); });
    addAction(QStringLiteral("folder-expand"), [this]() { actionExpand(); });
    addAction(QStringLiteral("next-change"), [this]() { nextDiff(true); });
    addAction(QStringLiteral("next-change-shortcut"), [this]() { nextDiff(true); });
    addAction(QStringLiteral("next-pane"), [this]() {
        const int pane = focusedPane();
        if (pane != -1)
            changeFocusedTree(pane, (pane + 1) % numPanes_);
    });
    addAction(QStringLiteral("open-external"), [this]() { actionOpenExternal(); });
    addAction(QStringLiteral("previous-change"), [this]() { nextDiff(false); });
    addAction(QStringLiteral("previous-change-shortcut"), [this]() { nextDiff(false); });
    addAction(QStringLiteral("previous-pane"), [this]() {
        const int pane = focusedPane();
        if (pane != -1)
            changeFocusedTree(pane, (pane - 1 + numPanes_) % numPanes_);
    });
    addAction(QStringLiteral("refresh"), [this]() { refresh(); });
    addAction(QStringLiteral("copy-file-paths"), [this]() { actionCopyFilePaths(); });

    addToggleAction(QStringLiteral("folder-filter"), false, {});
    for (const char *name : {"folder-status-same", "folder-status-new", "folder-status-modified"})
        addToggleAction(QLatin1String(name), false, [this](bool) { actionFilterStateChange(); });
    addToggleAction(QStringLiteral("folder-ignore-case"), false, [this](bool) { refresh(); });
    addToggleAction(QStringLiteral("folder-normalize-encoding"), false, [this](bool) { refresh(); });

    action(QStringLiteral("folder-status-same"))->setText(tr("Same"));
    action(QStringLiteral("folder-status-new"))->setText(tr("New"));
    action(QStringLiteral("folder-status-modified"))->setText(tr("Modified"));
    action(QStringLiteral("folder-ignore-case"))->setText(tr("Ignore Filename Case"));
    action(QStringLiteral("folder-normalize-encoding"))->setText(tr("Normalize Unicode Paths"));
}

QList<QPair<QString, QString>> DirDiff::fileFilterActions() const
{
    QList<QPair<QString, QString>> result;
    for (int i = 0; i < nameFilters_.size(); ++i)
        result.append({folderFilterActionName(i), nameFilters_.at(i).label});
    return result;
}

bool DirDiff::createNameFilters()
{
    MeldSettings *settings = MeldSettings::instance();

    // Ordering of name filters is irrelevant
    QSet<QString> oldActive;
    for (const FilterEntry &f : std::as_const(nameFilters_)) {
        if (f.active)
            oldActive.insert(f.filterString);
    }
    QSet<QString> newActive;
    for (const FilterEntry &f : settings->fileFilters()) {
        if (f.active)
            newActive.insert(f.filterString);
    }
    const bool activeFiltersChanged = oldActive != newActive;

    for (int i = 0; i < nameFilters_.size(); ++i)
        removeAction(folderFilterActionName(i));
    nameFilters_ = settings->fileFilters();
    for (int i = 0; i < nameFilters_.size(); ++i) {
        QAction *a = addToggleAction(folderFilterActionName(i), nameFilters_.at(i).active, [this, i](bool checked) {
            if (i < nameFilters_.size()) {
                nameFilters_[i].active = checked;
                refresh();
            }
        });
        a->setText(nameFilters_.at(i).label);
        a->setEnabled(nameFilters_.at(i).filter.has_value());
    }
    return activeFiltersChanged;
}

bool DirDiff::createTextFilters()
{
    MeldSettings *settings = MeldSettings::instance();
    // In contrast to file filters, ordering of text filters can matter
    QStringList oldActive;
    for (const FilterEntry &f : std::as_const(textFilters_)) {
        if (f.active)
            oldActive.append(f.filterString);
    }
    QStringList newActive;
    for (const FilterEntry &f : settings->textFilters()) {
        if (f.active)
            newActive.append(f.filterString);
    }
    textFilters_ = settings->textFilters();
    return oldActive != newActive;
}

void DirDiff::updateComparator()
{
    MeldSettings *settings = MeldSettings::instance();
    comparisonArgs_.shallowComparison = settings->boolValue(QStringLiteral("folder-shallow-comparison"));
    comparisonArgs_.timeResolutionNs = settings->intValue(QStringLiteral("folder-time-resolution"));
    comparisonArgs_.applyTextFilters = settings->boolValue(QStringLiteral("folder-filter-text"));
    comparisonArgs_.ignoreBlankLines = settings->boolValue(QStringLiteral("ignore-blank-lines"));
}

void DirDiff::updateTreeviewColumns()
{
    // Update the visibility and order of columns
    const QList<ColumnSetting> columns = MeldSettings::instance()->folderColumns();
    bool haveExtraColumns = false;
    for (const ColumnSetting &column : columns)
        haveExtraColumns = haveExtraColumns || column.visible;

    for (int i = 0; i < 3; ++i) {
        DirTreeView *view = treeview_[size_t(i)];
        if (!model_)
            continue;
        QHeaderView *header = view->header();
        for (int column = 0; column < model_->columnCount(); ++column)
            view->setColumnHidden(column, true);
        if (i >= model_->ntree())
            continue;

        const int nameColumn = DirDiffTreeModel::columnIndex(DirDiffTreeModel::ColName, i);
        view->setColumnHidden(nameColumn, false);
        view->setTreePosition(nameColumn);
        header->moveSection(header->visualIndex(nameColumn), 0);
        int lastVisual = 0;
        for (const ColumnSetting &setting : columns) {
            std::optional<DirDiffTreeModel::Column> column;
            for (auto c : {DirDiffTreeModel::ColSize, DirDiffTreeModel::ColModificationTime,
                           DirDiffTreeModel::ColModificationTimeIso, DirDiffTreeModel::ColPermissions}) {
                if (DirDiffTreeModel::columnKey(c) == setting.name)
                    column = c;
            }
            if (!column)
                continue;
            const int logical = DirDiffTreeModel::columnIndex(*column, i);
            view->setColumnHidden(logical, !setting.visible);
            header->moveSection(header->visualIndex(logical), ++lastVisual);
        }
        header->setSectionResizeMode(nameColumn, QHeaderView::Stretch);
        for (int c = 1; c < DirDiffTreeModel::ColumnsPerPane; ++c)
            header->setSectionResizeMode(nameColumn + c, QHeaderView::ResizeToContents);
        view->setHeaderHidden(!haveExtraColumns);
    }
}

void DirDiff::setNumPanes(int numPanes)
{
    if (numPanes == numPanes_ || numPanes < 1 || numPanes > 3)
        return;

    auto *oldModel = model_;
    model_ = new DirDiffTreeModel(numPanes, this);
    for (int i = 0; i < 3; ++i) {
        DirTreeView *view = treeview_[size_t(i)];
        view->setModel(model_);
        connect(view->selectionModel(), &QItemSelectionModel::currentChanged, this, [this, i]() { onCursorChanged(i); });
        connect(view->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this, i]() {
            if (treeview_[size_t(i)]->hasFocus())
                updateActionSensitivity();
        });
    }
    for (TreeViewChunkMap *chunkmap : chunkmap_) {
        connect(model_, &QAbstractItemModel::rowsInserted, chunkmap, &ChunkMap::invalidate);
        connect(model_, &QAbstractItemModel::rowsRemoved, chunkmap, &ChunkMap::invalidate);
        connect(model_, &QAbstractItemModel::dataChanged, chunkmap, &ChunkMap::invalidate);
        connect(model_, &QAbstractItemModel::modelReset, chunkmap, &ChunkMap::invalidate);
    }
    connect(model_, &QAbstractItemModel::rowsAboutToBeRemoved, this,
            [this](const QModelIndex &parent, int first, int last) {
                // Keep a sensible cursor when the current row is removed
                const QModelIndex current = currentPath_;
                if (!current.isValid())
                    return;
                for (int row = first; row <= last; ++row) {
                    if (current == model_->index(row, 0, parent)) {
                        QModelIndex replacement = row > 0 ? model_->index(row - 1, 0, parent) : parent;
                        if (!replacement.isValid() && last + 1 < model_->rowCount(parent))
                            replacement = model_->index(last + 1, 0, parent);
                        currentPath_ = replacement;
                        break;
                    }
                }
            });
    delete oldModel;

    for (int i = 0; i < 3; ++i) {
        const bool visible = i < numPanes;
        vbox_[size_t(i)]->setVisible(visible);
        paneActionbar_[size_t(i)]->setVisible(visible);
        chunkmap_[size_t(i)]->setVisible(visible);
    }
    for (int i = 0; i < 2; ++i) {
        linkmap_[size_t(i)]->setVisible(i < numPanes - 1);
        dummyToolbarLinkmap_[size_t(i)]->setVisible(i < numPanes - 1);
    }
    // Collapse the grid columns of hidden panes
    static constexpr int paneColumns[3] = {0, 2, 4};
    for (int i = 0; i < 3; ++i)
        grid_->setColumnStretch(paneColumns[i], i < numPanes ? 1 : 0);
    numPanes_ = numPanes;
    updateTreeviewColumns();
}

// ---------------------------------------------------------------------------
// Locations and scanning

void DirDiff::setFolders(const QStringList &folders)
{
    folders_ = folders;
}

void DirDiff::setLocations()
{
    QStringList locations;
    for (const QString &folder : std::as_const(folders_))
        locations.append(folder.isEmpty() ? QString() : QDir::cleanPath(QFileInfo(folder).absoluteFilePath()));
    if (locations.isEmpty())
        return;

    setNumPanes(int(locations.size()));

    QList<std::optional<QString>> parents;
    for (const QString &location : std::as_const(locations))
        parents.append(location.isEmpty() ? std::nullopt : std::optional<QString>(location));
    const std::optional<QString> parentPath = findSharedParentPath(parents);
    for (int pane = 0; pane < locations.size(); ++pane) {
        folderLabel_[size_t(pane)]->setParentFile(std::nullopt);
        folderLabel_[size_t(pane)]->setFile(locations.at(pane));
        folderLabel_[size_t(pane)]->setParentFile(parentPath);
        folderOpenButton_[size_t(pane)]->setFile(locations.at(pane));
    }

    currentPath_ = QPersistentModelIndex();
    marked_.reset();
    scheduler_.removeAllTasks();
    scanInProgress_ = 0;
    model_->clear();
    for (MsgAreaController *m : msgarea_)
        m->clear();
    TreeNode *child = model_->addEntries(nullptr, locations);
    onFocusIn(-1);
    updateItemState(child);
    recomputeLabel();
    recursivelyUpdate(model_->indexOf(child));
}

std::optional<Comparison> DirDiff::comparison() const
{
    // Comparisons with patches aren't recent comparisons
    if (patched_)
        return std::nullopt;
    Comparison c;
    c.type = RecentType::Folder;
    if (model_ && model_->firstRoot())
        c.paths = model_->valuePaths(model_->firstRoot());
    // Extracted archives are recorded as the archives
    for (QString &path : c.paths) {
        if (archive::isExtractedArchiveRoot(path))
            path = archive::formatArchivePath(path);
    }
    return c;
}

void DirDiff::markInProgressRow(TreeNode *node)
{
    // Mark a tree row as having a scan in progress. After the scan is
    // finished, updateItemState() must be called on the row to restore its
    // actual state.
    for (int pane = 0; pane < model_->ntree(); ++pane) {
        const QString folderName = node->panes[size_t(pane)].text;
        model_->setState(node, pane, StateSpinner, tr("%1 (scanning…)").arg(folderName), true);
        model_->setStats(node, pane, -1, MissingTimestamp, -1);
    }
}

void DirDiff::recursivelyUpdate(const QModelIndex &index)
{
    // Recursively update from tree path 'path'.
    TreeNode *node = model_->node(index);
    if (!node || node == model_->invisibleRoot())
        return;
    model_->removeChildren(node);
    if (scanInProgress_ == 0) {
        // Starting a scan, so set up progress indicator
        markInProgressRow(node);
    } else {
        updateItemState(node);
    }
    ++scanInProgress_;
    scheduler_.addTask(searchRecursivelyIter(QPersistentModelIndex(index)));
}

QList<QRegularExpression> DirDiff::activeByteFilters() const
{
    QList<QRegularExpression> regexes;
    for (const FilterEntry &f : textFilters_) {
        if (f.active && f.byteFilter)
            regexes.append(*f.byteFilter);
    }
    return regexes;
}

FileCompareResult DirDiff::fileCompare(const QStringList &files) const
{
    return filesSame(files, activeByteFilters(), comparisonArgs_);
}

// ---------------------------------------------------------------------------
// Patch comparisons

void DirDiff::setPatch(std::shared_ptr<patch::PatchedFolder> patched)
{
    patched_ = std::move(patched);
}

void DirDiff::reloadPatch(const QString &folder)
{
    if (!patched_)
        return;
    QString error;
    std::shared_ptr<patch::PatchedFolder> reloaded = patch::PatchedFolder::load(folder, patched_->patchFile(), &error);
    if (!reloaded) {
        msgarea_[1]->addDismissableMsg(
            QStringLiteral("dialog-error-symbolic"),
            tr("The patch “%1” can’t be read.").arg(QDir::toNativeSeparators(patched_->patchFile())), error);
        return;
    }
    patched_ = std::move(reloaded);
}

std::optional<QString> DirDiff::patchedPath(const QString &path) const
{
    if (!patched_ || path.isEmpty())
        return std::nullopt;
    const QString &root = patched_->patchFile();
    if (path == root)
        return QString();
    if (path.startsWith(root) && path.at(root.size()) == QLatin1Char('/'))
        return path.mid(root.size() + 1);
    return std::nullopt;
}

bool DirDiff::pathExists(const QString &path) const
{
    if (const std::optional<QString> patched = patchedPath(path))
        return patched_->exists(*patched);
    return exists(path);
}

bool DirDiff::pathIsDir(const QString &path) const
{
    if (const std::optional<QString> patched = patchedPath(path))
        return patched_->isDir(*patched);
    return isDir(path);
}

bool DirDiff::pathIsFile(const QString &path) const
{
    if (const std::optional<QString> patched = patchedPath(path))
        return patched_->isFile(*patched);
    return !path.isEmpty() && QFileInfo(path).isFile();
}

int DirDiff::patchedState(const QStringList &files) const
{
    // Files are the same as in the folder, unless the patch changes them;
    // the patched text is only made when they're compared
    const QString left = files.value(0);
    const std::optional<QString> right = patchedPath(files.value(1));
    if (!pathExists(left) || !right || !patched_->exists(*right))
        return StateNew;
    if (patched_->isDir(*right))
        return StateNormal;
    return patched_->change(*right) == patch::PatchedFolder::Change::None ? StateNormal : StateModified;
}

bool DirDiff::updatePatchedItemState(TreeNode *node)
{
    const QStringList files = model_->valuePaths(node);
    const bool isRoot = node->parent && !node->parent->parent;
    const int state = patchedState(files);
    std::array<bool, 2> present{};
    std::array<bool, 2> dirs{};
    for (int j = 0; j < 2; ++j) {
        present[size_t(j)] = pathExists(files.value(j));
        dirs[size_t(j)] = pathIsDir(files.value(j));
    }
    const bool anyDir = dirs[0] || dirs[1];

    for (int j = 0; j < 2; ++j) {
        const QString &path = files.value(j);
        if (!present[size_t(j)]) {
            model_->setPathState(node, j, StateNonExist, anyDir);
            model_->setStats(node, j, -1, MissingTimestamp, -1);
            continue;
        }
        QString name;
        if (isRoot)
            name = j == 0 ? archive::formatArchivePath(path) : path;
        model_->setPathState(node, j, state, dirs[size_t(j)], name);

        // Patched files' sizes and times aren't known until they're made
        QString real = path;
        if (const std::optional<QString> patched = patchedPath(path)) {
            real = patched_->change(*patched) == patch::PatchedFolder::Change::None
                ? QDir(patched_->folder()).filePath(*patched)
                : QString();
            if (patched->isEmpty())
                real = patched_->folder();
        }
        const QFileInfo info(real);
        if (!real.isEmpty() && info.exists()) {
            model_->setStats(node, j, dirs[size_t(j)] ? 4096 : info.size(), info.lastModified().toMSecsSinceEpoch(),
                             unixPermissions(info.permissions()));
        } else {
            model_->setStats(node, j, -1, MissingTimestamp, -1);
        }
    }
    return state != StateNormal;
}

TaskGenerator DirDiff::searchRecursivelyIter(QPersistentModelIndex rootIndex)
{
    for (DirTreeView *view : treeview_)
        view->selectionModel()->clearSelection();

    co_yield tr("Scanning %1").arg(QString());
    if (!rootIndex.isValid())
        co_return;

    const int prefixlen = 1 + int(model_->valuePath(model_->node(rootIndex), 0).size());
    QSet<QString> symlinksFollowed;
    std::vector<QPersistentModelIndex> todo{rootIndex};
    std::vector<QPersistentModelIndex> expanded;
    std::vector<std::pair<int, QString>> invalidFilenames;
    std::vector<std::tuple<int, QString, QString, QString>> shadowedEntries;
    std::vector<std::pair<int, QString>> whitespaceFilenames;

    const ComparisonOptions options{actionState(QStringLiteral("folder-ignore-case")),
                                    actionState(QStringLiteral("folder-normalize-encoding"))};

    while (!todo.empty()) {
        // Depth first: children are pushed in reverse order
        const QPersistentModelIndex path = todo.back();
        todo.pop_back();
        if (!path.isValid())
            continue;
        QStringList roots = model_->valuePaths(model_->node(path));

        // Buggy ordering when deleting rows means that we sometimes try to
        // recursively update files; this fix seems the least invasive.
        if (std::none_of(roots.begin(), roots.end(), [this](const QString &r) { return pathIsDir(r); }))
            continue;

        co_yield tr("Scanning %1").arg(roots.at(0).mid(prefixlen));
        if (!path.isValid())
            continue;
        TreeNode *node = model_->node(path);
        roots = model_->valuePaths(node);
        bool differences = false;

        CanonicalListing dirs(numPanes_, options);
        CanonicalListing files(numPanes_, options);

        for (int pane = 0; pane < roots.size(); ++pane) {
            const QString &root = roots.at(pane);
            if (!pathIsDir(root))
                continue;

            if (const std::optional<QString> patched = patchedPath(root)) {
                // The patched folder: its folder's entries, as the patch
                // changes them
                QStringList entries = patched_->entries(*patched);
                for (const FilterEntry &f : std::as_const(nameFilters_)) {
                    if (!f.active || !f.filter)
                        continue;
                    entries.removeIf([&f](const QString &e) { return pyMatch(*f.filter, e); });
                }
                const QString prefix = patched->isEmpty() ? QString() : *patched + QLatin1Char('/');
                for (const QString &e : std::as_const(entries)) {
                    const QString child = prefix + e;
                    const QFileInfo real(QDir(patched_->folder()).filePath(child));
                    if (ignoreSymlinks_ && real.isSymLink() && patched_->change(child) == patch::PatchedFolder::Change::None)
                        continue;
                    if (patched_->isDir(child))
                        dirs.add(pane, e);
                    else if (patched_->isFile(child))
                        files.add(pane, e);
                }
                for (const auto &[name, message] : patched_->errors(*patched)) {
                    model_->addError(node, tr("%1: %2").arg(name, message), pane);
                    differences = true;
                }
                continue;
            }

            QDir dir(root);
            if (!dir.isReadable()) {
                model_->addError(node, tr("Permission denied"), pane);
                differences = true;
                continue;
            }
            QStringList entries = dir.entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                                                QDir::Unsorted);

            for (const FilterEntry &f : std::as_const(nameFilters_)) {
                if (!f.active || !f.filter)
                    continue;
                entries.removeIf([&f](const QString &e) { return pyMatch(*f.filter, e); });
            }

            for (const QString &e : std::as_const(entries)) {
                if (e.contains(QChar::ReplacementCharacter)) {
                    invalidFilenames.emplace_back(pane, joinPath(root, e));
                    continue;
                }

                const QString full = joinPath(root, e);
                const QFileInfo info(full);
                if (info.isSymLink()) {
                    if (ignoreSymlinks_)
                        continue;
                    // Identify the link itself (like its inode), which
                    // stops us from following symlink loops
                    const QString key = joinPath(QFileInfo(root).canonicalFilePath(), e);
                    if (symlinksFollowed.contains(key))
                        continue;
                    symlinksFollowed.insert(key);
                    if (!info.exists()) {
                        model_->addError(node, e + tr(": Dangling symlink"), pane);
                        differences = true;
                    } else if (info.isFile()) {
                        files.add(pane, e);
                    } else if (info.isDir()) {
                        dirs.add(pane, e);
                    }
                } else if (info.isFile()) {
                    files.add(pane, e);
                } else if (info.isDir()) {
                    dirs.add(pane, e);
                }
                // FIXME: Unhandled stat type
            }
        }

        for (const auto &error : dirs.errors)
            shadowedEntries.emplace_back(error.pane, roots.at(error.pane), error.item, error.existing);
        for (const auto &error : files.errors)
            shadowedEntries.emplace_back(error.pane, roots.at(error.pane), error.item, error.existing);
        for (const auto &[pane, f] : dirs.whitespace)
            whitespaceFilenames.emplace_back(pane, joinPath(roots.at(pane), f));
        for (const auto &[pane, f] : files.whitespace)
            whitespaceFilenames.emplace_back(pane, joinPath(roots.at(pane), f));

        const std::vector<QStringList> alldirs = filterOnState(roots, dirs.get());
        const std::vector<QStringList> allfiles = filterOnState(roots, files.get());

        if (!alldirs.empty() || !allfiles.empty()) {
            std::vector<QPersistentModelIndex> childDirs;
            for (const QStringList &names : alldirs) {
                QStringList entriesPaths;
                for (int i = 0; i < roots.size(); ++i)
                    entriesPaths.append(joinPath(roots.at(i), names.value(i)));
                TreeNode *child = model_->addEntries(node, entriesPaths);
                differences = updateItemState(child) || differences;
                childDirs.emplace_back(model_->indexOf(child));
            }
            for (const QStringList &names : allfiles) {
                QStringList entriesPaths;
                for (int i = 0; i < roots.size(); ++i)
                    entriesPaths.append(joinPath(roots.at(i), names.value(i)));
                TreeNode *child = model_->addEntries(node, entriesPaths);
                differences = updateItemState(child) || differences;
            }
            for (auto it = childDirs.rbegin(); it != childDirs.rend(); ++it)
                todo.push_back(*it);
        } else {
            // Our subtree is empty, or has been filtered to be empty
            const bool allDirs = std::all_of(roots.begin(), roots.end(), isDir);
            if (std::find(stateFilters_.begin(), stateFilters_.end(), StateNormal) != stateFilters_.end() || !allDirs) {
                model_->addEmpty(node);
                if (node->parent == model_->invisibleRoot())
                    expanded.emplace_back(model_->indexOf(node));
            } else {
                // At this point, we have an empty folder tree node; we can
                // prune this and any ancestors that then end up empty.
                while (node->children.empty()) {
                    TreeNode *parent = node->parent;
                    // In our tree, there is always a top-level parent with no
                    // siblings. If we're here, we have an empty tree.
                    if (parent == model_->invisibleRoot()) {
                        model_->addEmpty(node);
                        break;
                    }
                    model_->removeNode(node);
                    node = parent;
                }
            }
        }

        if (differences && path.isValid())
            expanded.push_back(path);
    }

    QStringList rootPaths = rootIndex.isValid() ? model_->valuePaths(model_->node(rootIndex)) : QStringList();
    QString duplicate;
    for (int i = 0; i < rootPaths.size() && duplicate.isEmpty(); ++i) {
        for (int j = i + 1; j < rootPaths.size(); ++j) {
            if (!rootPaths.at(i).isEmpty() && rootPaths.at(i) == rootPaths.at(j))
                duplicate = rootPaths.at(i);
        }
    }

    if (!invalidFilenames.empty() || !shadowedEntries.empty() || !whitespaceFilenames.empty()) {
        showTreeWideErrors(invalidFilenames, shadowedEntries, whitespaceFilenames);
    } else if (!duplicate.isEmpty()) {
        // Since we can only load 3 dirs we can have at most 1 duplicate
        showDuplicateDirectory(duplicate);
    } else if (rootIndex.isValid() && !rootIndex.parent().isValid() && expanded.empty()) {
        showIdenticalStatus();
    }

    if (TreeNode *first = model_->firstRoot())
        treeview_[0]->expand(model_->indexOf(first));
    for (const QPersistentModelIndex &index : expanded) {
        if (index.isValid())
            expandToIndex(treeview_[0], index);
    }
    co_yield tr("Done");

    --scanInProgress_;
    if (scanInProgress_ == 0 && rootIndex.isValid()) {
        // Finishing a scan, so remove progress indicator
        updateItemState(model_->node(rootIndex));
    }

    forceCursorRecalculate_ = true;
    if (rootIndex.isValid())
        treeview_[0]->setCurrentIndex(rootIndex);
    updateActionSensitivity();
}

std::vector<QStringList> DirDiff::filterOnState(const QStringList &roots, const std::vector<QStringList> &fileslist)
{
    // Get state of 'files' for filtering purposes.
    std::vector<QStringList> ret;
    for (const QStringList &names : fileslist) {
        QStringList curfiles;
        for (int i = 0; i < roots.size(); ++i)
            curfiles.append(joinPath(roots.at(i), names.value(i)));
        std::vector<bool> isPresent;
        for (const QString &f : std::as_const(curfiles))
            isPresent.push_back(pathExists(f));
        const auto presentCount = std::count(isPresent.begin(), isPresent.end(), true);

        QSet<int> states;
        if (patched_) {
            states = {patchedState(curfiles)};
        } else if (presentCount == qsizetype(isPresent.size())) {
            const FileCompareResult result = fileCompare(curfiles);
            if (result == FileCompareResult::Same || result == FileCompareResult::DodgySame)
                states = {StateNormal};
            else if (result == FileCompareResult::SameFiltered)
                states = {StateNoChange};
            else
                states = {StateModified};
        } else if (presentCount > 1) {
            // In a three-way comparison, we can have files in e.g., pane 1
            // and 2 be different to each other, and there be no file in pane
            // 3. This row should be considered both modified (1 -> 2) and new
            // (2 -> 3).
            QStringList present;
            for (int i = 0; i < curfiles.size(); ++i) {
                if (isPresent[size_t(i)])
                    present.append(curfiles.at(i));
            }
            curfiles = present;
            const FileCompareResult result = fileCompare(curfiles);
            if (result == FileCompareResult::Same || result == FileCompareResult::DodgySame
                || result == FileCompareResult::SameFiltered) {
                states = {StateNew};
            } else {
                states = {StateNew, StateModified};
            }
        } else {
            states = {StateNew};
        }

        // Always retain NORMAL folders for comparison; we remove these later
        // if they have no children.
        const bool allFolders =
            std::all_of(curfiles.begin(), curfiles.end(), [this](const QString &f) { return pathIsDir(f); });
        bool statesMatchFilters = false;
        for (int state : stateFilters_)
            statesMatchFilters = statesMatchFilters || states.contains(state);
        if (statesMatchFilters || allFolders)
            ret.push_back(names);
    }
    return ret;
}

bool DirDiff::updateItemState(TreeNode *node)
{
    // Update the state of a tree row. All changes and updates to tree rows
    // should happen here; structural changes happen elsewhere, but they only
    // delete rows or add new rows with path information.
    if (patched_)
        return updatePatchedItemState(node);

    const QStringList files = model_->valuePaths(node);
    const int ntree = model_->ntree();

    std::vector<std::optional<QFileInfo>> stats;
    std::vector<bool> symlinks;
    for (int i = 0; i < ntree; ++i) {
        const QString &f = files.at(i);
        const QFileInfo info(f);
        stats.push_back(!f.isEmpty() && info.exists() ? std::optional<QFileInfo>(info) : std::nullopt);
        symlinks.push_back(!f.isEmpty() && info.isSymLink());
    }

    const bool isRoot = node->parent && !node->parent->parent;
    const auto nameOverride = [&](int i) -> QString {
        if (symlinks[size_t(i)]) {
            const QFileInfo info(files.at(i));
            return QStringLiteral("%1 ⟶ %2").arg(info.fileName(), info.symLinkTarget());
        }
        if (isRoot)
            return archive::formatArchivePath(files.at(i));
        return {};
    };

    const bool allExist = std::all_of(stats.begin(), stats.end(), [](const auto &s) { return s.has_value(); });
    FileCompareResult allSame;
    FileCompareResult allPresentSame;
    if (allExist) {
        allSame = fileCompare(files);
        allPresentSame = allSame;
    } else {
        QStringList lof;
        for (int i = 0; i < ntree; ++i) {
            if (stats[size_t(i)])
                lof.append(files.at(i));
        }
        allSame = FileCompareResult::Different;
        allPresentSame = fileCompare(lof);
    }

    int state;
    using R = FileCompareResult;
    // TODO: Differentiate the DodgySame case
    if (allSame == R::Same || allSame == R::DodgySame)
        state = StateNormal;
    else if (allSame == R::SameFiltered)
        state = StateNoChange;
    // TODO: Differentiate the SameFiltered and DodgySame cases
    else if (allPresentSame == R::Same || allPresentSame == R::SameFiltered || allPresentSame == R::DodgySame)
        state = StateNew;
    else if (allSame == R::FileError || allPresentSame == R::FileError)
        state = StateError;
    // Different and DodgyDifferent
    else
        state = StateModified;
    const bool different = state != StateNormal && state != StateNoChange;

    std::vector<bool> dirFlags;
    for (int j = 0; j < ntree; ++j)
        dirFlags.push_back(isDir(files.at(j)));
    const bool anyDir = std::any_of(dirFlags.begin(), dirFlags.end(), [](bool b) { return b; });

    for (int j = 0; j < ntree; ++j) {
        if (stats[size_t(j)]) {
            const QFileInfo &info = *stats[size_t(j)];
            model_->setPathState(node, j, state, dirFlags[size_t(j)], nameOverride(j));
            model_->setStats(node, j, dirFlags[size_t(j)] ? 4096 : info.size(), info.lastModified().toMSecsSinceEpoch(),
                             unixPermissions(info.permissions()));
            if (marked_ && marked_->pane == j && marked_->row == model_->indexOf(node))
                model_->setIcon(node, j, QStringLiteral("checkbox-checked-symbolic"));
            else if (symlinks[size_t(j)])
                model_->setIcon(node, j, QStringLiteral("symbolic-link-symbolic"));
        } else {
            model_->setPathState(node, j, StateNonExist, anyDir);
            model_->setStats(node, j, -1, MissingTimestamp, -1);
        }
    }
    return different;
}

void DirDiff::showIdenticalStatus()
{
    const QString primary = tr("Folders have no differences");
    const QString identicalNote = tr("Contents of scanned files in folders are identical.");
    const QString shallowNote = tr("Scanned files in folders appear identical, but contents have not been scanned.");
    const QString fileFilterQualifier = tr("File filters are in use, so not all files have been scanned.");
    const QString textFilterQualifier = tr("Text filters are in use and may be masking content differences.");

    const bool isShallow = comparisonArgs_.shallowComparison;
    bool haveFileFilters = false;
    for (const FilterEntry &f : std::as_const(nameFilters_))
        haveFileFilters = haveFileFilters || f.active;
    bool haveTextFilters = false;
    for (const FilterEntry &f : std::as_const(textFilters_))
        haveTextFilters = haveTextFilters || f.active;

    QStringList secondary{isShallow ? shallowNote : identicalNote};
    if (haveFileFilters)
        secondary.append(fileFilterQualifier);
    if (!isShallow && haveTextFilters)
        secondary.append(textFilterQualifier);

    for (int pane = 0; pane < numPanes_; ++pane) {
        MsgArea *msgarea = msgarea_[size_t(pane)]->newFromTextAndIcon(primary, secondary.join(QLatin1Char(' ')));
        msgarea->addButton(pane == 0 ? tr("Hi&de") : tr("Hide"), Response::Close);
        connect(msgarea, &MsgArea::response, this, [this]() {
            for (int p = 0; p < numPanes_; ++p)
                msgarea_[size_t(p)]->clear();
        });
    }
}

void DirDiff::showDuplicateDirectory(const QString &directory)
{
    QList<MsgAreaController *> panes(msgarea_.begin(), msgarea_.end());
    for (int index = 0; index < numPanes_; ++index) {
        msgarea_[size_t(index)]->addDismissableMsg(QStringLiteral("dialog-warning-symbolic"),
                                                   tr("Folder %1 is being compared to itself").arg(directory),
                                                   QString(), panes);
    }
}

void DirDiff::showTreeWideErrors(const std::vector<std::pair<int, QString>> &invalid,
                                 const std::vector<std::tuple<int, QString, QString, QString>> &shadowed,
                                 const std::vector<std::pair<int, QString>> &whitespace)
{
    QString header = tr("Multiple errors occurred while scanning this folder");
    const QString invalidHeader = tr("Files with invalid encodings found");
    const QString invalidSecondary = tr("Some files were in an incorrect encoding. The names are something like:");
    const QString shadowedHeader = tr("Files hidden by case insensitive comparison");
    const QString shadowedSecondary = tr("You are running a case insensitive comparison on a case sensitive "
                                         "filesystem. The following files in this folder are hidden:");
    const QString whitespaceHeader = tr("Files had mismatched leading or trailing whitespace");
    const QString whitespaceSecondary = tr("This comparison found some files that differed only in leading or "
                                           "trailing whitespace. Their names appear as:");

    std::vector<QStringList> invalidEntries(static_cast<size_t>(numPanes_));
    for (const auto &[pane, path] : invalid)
        invalidEntries[size_t(pane)].append(path);
    std::vector<QStringList> formattedEntries(static_cast<size_t>(numPanes_));
    for (const auto &[pane, root, f1, f2] : shadowed) {
        formattedEntries[size_t(pane)].append(
            tr("“%1” hidden by “%2”").arg(joinPath(root, f1), joinPath(root, f2)));
    }
    std::vector<QStringList> whitespaceEntries(static_cast<size_t>(numPanes_));
    for (const auto &entry : whitespace) {
        for (int pane = 0; pane < numPanes_; ++pane)
            whitespaceEntries[size_t(pane)].append(entry.second);
    }

    for (int pane = 0; pane < numPanes_; ++pane) {
        const QString invalidText = invalidEntries[size_t(pane)].join(QLatin1Char('\n'));
        const QString shadowedText = formattedEntries[size_t(pane)].join(QLatin1Char('\n'));
        const QString whitespaceText = whitespaceEntries[size_t(pane)].join(QLatin1Char('\n'));

        const int count = int(!invalidText.isEmpty()) + int(!shadowedText.isEmpty()) + int(!whitespaceText.isEmpty());
        if (count == 0)
            continue;
        QString paneHeader = header;
        if (count == 1) {
            if (!invalidText.isEmpty())
                paneHeader = invalidHeader;
            else if (!shadowedText.isEmpty())
                paneHeader = shadowedHeader;
            else
                paneHeader = whitespaceHeader;
        }

        QStringList messages;
        if (!invalidText.isEmpty())
            messages << invalidSecondary << invalidText << QString();
        if (!shadowedText.isEmpty())
            messages << shadowedSecondary << shadowedText << QString();
        if (!whitespaceText.isEmpty())
            messages << whitespaceSecondary << whitespaceText << QString();

        msgarea_[size_t(pane)]->addDismissableMsg(QStringLiteral("dialog-error-symbolic"), paneHeader,
                                                  messages.join(QLatin1Char('\n')).trimmed());
    }
}

void DirDiff::recomputeLabel()
{
    TreeNode *root = model_->firstRoot();
    QStringList filenames = root ? model_->valuePaths(root) : QStringList();
    for (QString &f : filenames) {
        if (const QString archive = archive::formatArchivePath(f); !archive.isEmpty())
            f = archive;
        else if (f.isEmpty())
            f = tr("No folder");
    }
    QStringList shortnames;
    QStringList tooltipNames;
    if (!customLabels_.isEmpty()) {
        for (int i = 0; i < filenames.size(); ++i) {
            const QString custom = customLabels_.value(i);
            shortnames.append(custom.isEmpty() ? filenames.at(i) : custom);
        }
        tooltipNames = shortnames;
    } else {
        shortnames = shortenNames(filenames);
        tooltipNames = filenames;
    }
    setTabLabel(shortnames.join(QStringLiteral(" : ")),
                (QStringList{tr("Folder comparison:")} + tooltipNames).join(QLatin1Char('\n')));
}

void DirDiff::setLabels(const QStringList &labels)
{
    QStringList list = labels.mid(0, numPanes_);
    for (int i = 0; i < list.size(); ++i) {
        if (!list.at(i).isEmpty())
            folderLabel_[size_t(i)]->setCustomLabel(list.at(i));
    }
    while (list.size() < numPanes_)
        list.append(QString());
    customLabels_ = list;
    recomputeLabel();
}

void DirDiff::refresh()
{
    // The patch is read again, as are the folders
    if (patched_)
        reloadPatch(patched_->folder());
    setLocations();
}

void DirDiff::autoCompare()
{
    for (TreeNode *node : model_->stateRows({StateModified, StateConflict}))
        runDiffFromNode(node);
}

void DirDiff::requestClose()
{
    // Archives extracted for this comparison go with it. File comparisons
    // opened from it keep their loaded files, which can't be saved anyway.
    if (TreeNode *root = model_->firstRoot()) {
        QStringList archives;
        for (const QString &path : model_->valuePaths(root)) {
            if (archive::isExtractedArchiveRoot(path))
                archives.append(path);
        }
        archive::removeExtractedArchives(archives);
    }
    MeldDoc::requestClose();
}

void DirDiff::onFileChanged(const QString &changedFilename)
{
    // When a file has changed, try to find it in our tree and update its
    // status if necessary
    TreeNode *root = model_->firstRoot();
    if (!root)
        return;
    QList<TreeNode *> changedNodes;
    const QStringList changed = QDir::cleanPath(changedFilename).split(QLatin1Char('/'));
    for (int pane = 0; pane < numPanes_; ++pane) {
        const QStringList current = model_->valuePath(root, pane).split(QLatin1Char('/'));
        if (current.isEmpty() || changed.mid(0, current.size()) != current)
            continue;
        TreeNode *it = root;
        for (const QString &part : changed.mid(current.size())) {
            TreeNode *found = nullptr;
            for (const auto &child : it->children) {
                const QString childPath = model_->valuePath(child.get(), pane);
                if (!childPath.isEmpty() && part == path::basename(childPath)) {
                    found = child.get();
                    break;
                }
            }
            it = found;
            if (!it)
                break;
        }
        if (it && !changedNodes.contains(it))
            changedNodes.append(it);
    }
    for (TreeNode *node : std::as_const(changedNodes))
        updateItemState(node);
    forceCursorRecalculate_ = true;
}

// ---------------------------------------------------------------------------
// Selection and focus

int DirDiff::focusedPane() const
{
    const QWidget *focus = window() ? window()->focusWidget() : nullptr;
    for (int i = 0; i < numPanes_; ++i) {
        if (focus == treeview_[size_t(i)])
            return i;
    }
    return -1;
}

QWidget *DirDiff::focusPane() const
{
    return focusPane_ ? static_cast<QWidget *>(focusPane_.data()) : treeview_[0];
}

QList<TreeNode *> DirDiff::selectedNodes(int pane) const
{
    QList<TreeNode *> nodes;
    for (const QModelIndex &index : treeview_[size_t(pane)]->selectionModel()->selectedIndexes()) {
        TreeNode *node = model_->node(index);
        if (!nodes.contains(node))
            nodes.append(node);
    }
    // Keep tree order, as GtkTreeSelection does
    std::sort(nodes.begin(), nodes.end(), [this](TreeNode *a, TreeNode *b) {
        const auto pathOf = [this](TreeNode *n) {
            QList<int> rows;
            for (TreeNode *p = n; p && p != model_->invisibleRoot(); p = p->parent)
                rows.prepend(p->row());
            return rows;
        };
        return pathOf(a) < pathOf(b);
    });
    return nodes;
}

void DirDiff::updateActionSensitivity()
{
    int pane = focusedPane();
    if (pane == -1 && focusPane_) {
        for (int i = 0; i < numPanes_; ++i) {
            if (treeview_[size_t(i)] == focusPane_)
                pane = i;
        }
    }

    const QList<TreeNode *> selected = pane != -1 ? selectedNodes(pane) : QList<TreeNode *>();
    if (!selected.isEmpty()) {
        bool isValid = true;
        for (TreeNode *node : selected) {
            const int state = model_->state(node, pane);
            if (state == StateError || state == StateNonExist) {
                isValid = false;
                break;
            }
        }
        const bool busy = scanInProgress_ > 0;
        isValid = isValid && !busy;

        const bool isSingleFoldableRow = selected.size() == 1 && !selected.first()->children.empty();
        setActionEnabled(QStringLiteral("folder-collapse"), isSingleFoldableRow);
        setActionEnabled(QStringLiteral("folder-expand"), isSingleFoldableRow);
        setActionEnabled(QStringLiteral("folder-compare"), true);
        setActionEnabled(QStringLiteral("folder-mark"), true);
        setActionEnabled(QStringLiteral("folder-compare-marked"), marked_.has_value() && marked_->pane != pane);
        setActionEnabled(QStringLiteral("swap-2-panes"), numPanes_ == 2);
        setActionEnabled(QStringLiteral("folder-delete"), isValid);
        setActionEnabled(QStringLiteral("folder-copy-left"), isValid && pane > 0);
        setActionEnabled(QStringLiteral("folder-copy-right"), isValid && pane + 1 < numPanes_);
        setActionEnabled(QStringLiteral("open-external"), isValid);
    } else {
        for (const char *name : {"folder-collapse", "folder-compare", "folder-mark", "folder-compare-marked",
                                 "folder-copy-left", "folder-copy-right", "folder-delete", "folder-expand",
                                 "open-external"}) {
            setActionEnabled(QLatin1String(name), false);
        }
        setActionEnabled(QStringLiteral("swap-2-panes"), numPanes_ == 2);
    }

    // The patched folder can't be changed, copied from or marked; its files
    // are only made in comparisons
    if (patched_) {
        for (const char *name : {"folder-mark", "folder-compare-marked", "folder-copy-left", "folder-copy-right",
                                 "swap-2-panes"}) {
            setActionEnabled(QLatin1String(name), false);
        }
        if (pane == 1) {
            setActionEnabled(QStringLiteral("folder-delete"), false);
            setActionEnabled(QStringLiteral("open-external"), false);
        }
    }
}

void DirDiff::onCursorChanged(int pane)
{
    if (model_->rowCount() == 0)
        return;
    const QModelIndex cursorIndex = treeview_[size_t(pane)]->currentIndex();
    const QModelIndex cursorPath = cursorIndex.isValid() ? cursorIndex.siblingAtColumn(0) : QModelIndex();
    if (!cursorPath.isValid()) {
        setActionEnabled(QStringLiteral("previous-change"), false);
        setActionEnabled(QStringLiteral("next-change"), false);
        currentPath_ = QPersistentModelIndex();
        return;
    }

    bool skip = false;
    if (forceCursorRecalculate_) {
        // We force cursor recalculation on initial load, and when we handle
        // model change events.
        forceCursorRecalculate_ = false;
    } else if (currentPath_.isValid()) {
        // We can skip recalculation if the new cursor is between the
        // previous/next bounds, and we weren't on a changed row
        const int state = model_->state(model_->node(currentPath_), 0);
        if (state == StateNormal || state == StateNoChange || state == StateEmpty) {
            const auto before = [this](const QModelIndex &a, const QModelIndex &b) {
                for (TreeNode *n = model_->node(a); n; n = model_->inorderNext(n)) {
                    if (n == model_->node(b))
                        return n != model_->node(a);
                }
                return false;
            };
            if (!prevPath_.isValid() && !nextPath_.isValid())
                skip = true;
            else if (!prevPath_.isValid())
                skip = before(cursorPath, nextPath_);
            else if (!nextPath_.isValid())
                skip = before(prevPath_, cursorPath);
            else
                skip = before(prevPath_, cursorPath) && before(cursorPath, nextPath_);
        }
    }

    if (!skip) {
        const auto [prev, next] = model_->findNextPrevDiff(cursorPath);
        prevPath_ = prev;
        nextPath_ = next;
        setActionEnabled(QStringLiteral("previous-change"), prev.isValid());
        setActionEnabled(QStringLiteral("next-change"), next.isValid());
    }
    currentPath_ = cursorPath;
}

void DirDiff::changeFocusedTree(int oldPane, int newPane)
{
    DirTreeView *oldView = treeview_[size_t(oldPane)];
    DirTreeView *newView = treeview_[size_t(newPane)];
    const QList<TreeNode *> nodes = selectedNodes(oldPane);
    oldView->selectionModel()->clearSelection();

    newView->setFocus();
    newView->selectionModel()->clearSelection();
    if (!nodes.isEmpty()) {
        newView->setCurrentIndex(model_->indexOf(nodes.first(), DirDiffTreeModel::columnIndex(DirDiffTreeModel::ColName, newPane)));
        for (TreeNode *node : nodes)
            newView->selectionModel()->select(model_->indexOf(node), QItemSelectionModel::Select | QItemSelectionModel::Rows);
    }
    onCursorChanged(newPane);
    updateActionSensitivity();
}

void DirDiff::onRowActivated(int pane, const QModelIndex &index)
{
    TreeNode *node = model_->node(index);
    const QStringList rows = model_->valuePaths(node);
    // Click a file: compare; click a directory: expand; click a missing
    // entry: check the next neighbouring entry
    static constexpr int paneOrdering[3][3] = {{0, 1, 2}, {1, 2, 0}, {2, 1, 0}};
    for (int p : paneOrdering[pane]) {
        if (p < numPanes_ && pathExists(rows.value(p))) {
            pane = p;
            break;
        }
    }
    const QString path = rows.value(pane);
    if (path.isEmpty())
        return;
    if (pathIsFile(path)) {
        runDiffFromNode(node);
    } else if (pathIsDir(path)) {
        DirTreeView *view = treeview_[size_t(pane)];
        const QModelIndex row = index.siblingAtColumn(0);
        view->setExpanded(row, !view->isExpanded(row));
    }
}

void DirDiff::onRowExpanded(int pane, const QModelIndex &index)
{
    if (doToOthersLock_)
        return;
    doToOthersLock_ = true;
    for (int i = 0; i < numPanes_; ++i) {
        if (i != pane)
            treeview_[size_t(i)]->expand(index.siblingAtColumn(0));
    }
    doToOthersLock_ = false;
}

void DirDiff::onRowCollapsed(int pane, const QModelIndex &index)
{
    if (doToOthersLock_)
        return;
    doToOthersLock_ = true;
    for (int i = 0; i < numPanes_; ++i) {
        if (i != pane)
            treeview_[size_t(i)]->collapse(index.siblingAtColumn(0));
    }
    doToOthersLock_ = false;
}

void DirDiff::onFocusIn(int pane)
{
    QPointer<DirTreeView> oldPane = focusPane_;
    focusPane_ = pane >= 0 ? treeview_[size_t(pane)] : treeview_[0];
    updateActionSensitivity();
    if (oldPane && oldPane != focusPane_)
        oldPane->selectionModel()->clearSelection();
    for (int i = 0; i < 3; ++i) {
        if (treeview_[size_t(i)] == focusPane_ && model_ && model_->rowCount() > 0)
            onCursorChanged(i);
    }
}

void DirDiff::runDiffFromNode(TreeNode *node)
{
    const QStringList rows = model_->valuePaths(node);
    if (patched_) {
        // The file, and its patched text, which is made as it's compared
        const std::optional<QString> patched = patchedPath(rows.value(1));
        if (!patched || patched_->isDir(*patched) || (pathIsDir(rows.value(0))))
            return;
        if (!pathIsFile(rows.value(0)) && !patched_->exists(*patched))
            return;
        ComparisonMeta meta;
        meta.virtualFiles.insert(1, patchedFile(patched_, *patched));
        meta.tabLabel = patchComparisonTabLabel(*patched, patched_->patchFile());
        DiffOptions options;
        options.meta = meta;
        Q_EMIT createDiff({pathIsFile(rows.value(0)) ? rows.value(0) : QString(), QString()}, options);
        return;
    }
    QStringList existing;
    for (const QString &r : rows) {
        if (exists(r))
            existing.append(r);
    }
    if (existing.isEmpty())
        return;
    const bool isFile = QFileInfo(existing.first()).isFile();
    const bool isDirectory = QFileInfo(existing.first()).isDir();
    QStringList paths;
    if (isFile) {
        for (const QString &r : rows)
            paths.append(QFileInfo(r).isFile() ? r : QString());
    } else if (isDirectory) {
        for (const QString &r : rows)
            paths.append(QFileInfo(r).isDir() ? r : QString());
    } else {
        return;
    }
    Q_EMIT createDiff(paths, DiffOptions{});
}

void DirDiff::nextDiff(bool down)
{
    int pane = 0;
    for (int i = 0; i < numPanes_; ++i) {
        if (focusPane_ == treeview_[size_t(i)])
            pane = i;
    }
    const QPersistentModelIndex path = down ? nextPath_ : prevPath_;
    if (path.isValid()) {
        DirTreeView *view = treeview_[size_t(pane)];
        expandToIndex(view, path.parent());
        view->setCurrentIndex(model_->index(path.row(), DirDiffTreeModel::columnIndex(DirDiffTreeModel::ColName, pane),
                                            path.parent()));
        view->scrollTo(path);
    } else {
        errorBell();
    }
}

void DirDiff::showContextMenu(int pane, const QPoint &globalPos)
{
    onFocusIn(pane);
    updateActionSensitivity();
    QMenu menu(this);
    const auto add = [&](const QString &name, const QString &label) {
        QAction *a = action(name);
        QAction *item = menu.addAction(label);
        item->setEnabled(a->isEnabled());
        connect(item, &QAction::triggered, a, &QAction::trigger);
    };
    add(QStringLiteral("folder-compare"), tr("&Compare Selected Files"));
    menu.addSeparator();
    add(QStringLiteral("folder-mark"), tr("&Mark Selected File"));
    add(QStringLiteral("folder-compare-marked"), tr("Compare &with Marked File"));
    menu.addSeparator();
    add(QStringLiteral("folder-copy-left"), tr("Copy to &Left"));
    add(QStringLiteral("folder-copy-right"), tr("Copy to &Right"));
    add(QStringLiteral("folder-delete"), tr("&Delete Selected"));
    menu.addSeparator();
    add(QStringLiteral("folder-collapse"), tr("Collapse Recursively"));
    add(QStringLiteral("folder-expand"), tr("Expand Recursively"));
    menu.addSeparator();
    add(QStringLiteral("open-external"), tr("&Open Externally"));
    add(QStringLiteral("copy-file-paths"), tr("&Copy File Paths"));
    menu.exec(globalPos);
}

bool DirDiff::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == searchEntry_) {
        if (event->type() == QEvent::ShortcutOverride) {
            // Escape and Return belong to the search entry while it has
            // focus, rather than to the stop and folder-compare shortcuts
            const int key = static_cast<QKeyEvent *>(event)->key();
            if (key == Qt::Key_Escape || key == Qt::Key_Return || key == Qt::Key_Enter) {
                event->accept();
                return false;
            }
        }
        if (event->type() == QEvent::KeyPress && static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape) {
            searchEntry_->hide();
            if (focusPane_)
                focusPane_->setFocus();
            return true;
        }
        if (event->type() == QEvent::FocusOut)
            searchEntry_->hide();
    }
    return MeldDoc::eventFilter(watched, event);
}

void DirDiff::startSearch()
{
    searchEntry_->show();
    searchEntry_->setFocus();
    searchEntry_->selectAll();
}

// ---------------------------------------------------------------------------
// Actions

void DirDiff::actionDiff()
{
    int pane = focusedPane();
    if (pane == -1)
        return;
    for (TreeNode *node : selectedNodes(pane))
        runDiffFromNode(node);
}

void DirDiff::actionMark()
{
    const int pane = focusedPane();
    if (pane == -1)
        return;
    const QList<TreeNode *> selected = selectedNodes(pane);
    if (selected.isEmpty())
        return;

    const QPersistentModelIndex oldMark = marked_ ? marked_->row : QPersistentModelIndex();
    // We'll assume that in any multi-select, the first row was the intended
    // mark.
    marked_ = Marker{pane, QPersistentModelIndex(model_->indexOf(selected.first()))};
    updateItemState(selected.first());
    if (oldMark.isValid())
        updateItemState(model_->node(oldMark));
    updateActionSensitivity();
}

void DirDiff::actionDiffMarked()
{
    const int pane = focusedPane();
    if (pane == -1 || !marked_ || !marked_->row.isValid())
        return;
    const QList<TreeNode *> selected = selectedNodes(pane);
    if (selected.isEmpty())
        return;

    const QString markedPath = model_->valuePath(model_->node(marked_->row), marked_->pane);
    const QString selectedPath = model_->valuePath(selected.first(), pane);
    // Maintain the pane ordering in the new comparison, regardless of which
    // pane is the marked one.
    const QStringList rowPaths = pane < marked_->pane ? QStringList{selectedPath, markedPath}
                                                      : QStringList{markedPath, selectedPath};
    QStringList paths;
    for (const QString &p : rowPaths) {
        if (exists(p))
            paths.append(p);
    }
    Q_EMIT createDiff(paths, DiffOptions{});
}

void DirDiff::actionCollapse()
{
    const int pane = focusedPane();
    if (pane == -1)
        return;
    const QList<TreeNode *> selected = selectedNodes(pane);
    if (selected.isEmpty())
        return;
    std::vector<QModelIndex> toCollapse;
    std::function<void(TreeNode *)> collect = [&](TreeNode *n) {
        toCollapse.push_back(model_->indexOf(n));
        for (const auto &child : n->children)
            collect(child.get());
    };
    collect(selected.first());
    for (auto it = toCollapse.rbegin(); it != toCollapse.rend(); ++it)
        treeview_[size_t(pane)]->collapse(*it);
}

void DirDiff::actionExpand()
{
    const int pane = focusedPane();
    if (pane == -1)
        return;
    for (TreeNode *node : selectedNodes(pane))
        treeview_[size_t(pane)]->expandRecursively(model_->indexOf(node));
}

void DirDiff::fileCreated(TreeNode *node)
{
    TreeNode *root = model_->firstRoot();
    while (node && node != root && node != model_->invisibleRoot()) {
        updateItemState(node);
        node = node->parent;
    }
}

void DirDiff::fileDeleted(TreeNode *node, int pane)
{
    Q_UNUSED(pane);
    // is file still extant in other pane?
    const QStringList files = model_->valuePaths(node);
    if (std::any_of(files.begin(), files.end(), exists))
        updateItemState(node);
    else
        model_->removeNode(node);
}

void DirDiff::copySelected(int direction)
{
    const int srcPane = focusedPane();
    if (srcPane == -1)
        return;
    const int dstPane = srcPane + direction;
    if (dstPane < 0 || dstPane >= numPanes_)
        return;

    QList<QPersistentModelIndex> paths;
    for (TreeNode *node : selectedNodes(srcPane))
        paths.prepend(QPersistentModelIndex(model_->indexOf(node)));

    for (const QPersistentModelIndex &path : std::as_const(paths)) {
        if (!path.isValid())
            continue;
        TreeNode *node = model_->node(path);
        const QString src = model_->valuePath(node, srcPane);
        const QString dst = model_->valuePath(node, dstPane);
        if (src.isEmpty())
            continue;
        QString error;
        bool ok = true;
        if (QFileInfo(src).isFile()) {
            const QString dstDir = QFileInfo(dst).absolutePath();
            if (!QFileInfo::exists(dstDir) && !QDir().mkpath(dstDir)) {
                ok = false;
                error = tr("Couldn't create folder %1").arg(dstDir);
            } else {
                ok = copyFile(src, dstDir, &error);
                if (ok)
                    fileCreated(node);
            }
        } else if (QFileInfo(src).isDir()) {
            if (QFileInfo::exists(dst)) {
                const QFileInfo dstInfo(dst);
                QMessageBox box(QMessageBox::Warning, tr("Replace folder “%1”?").arg(dstInfo.fileName()),
                                tr("Replace folder “%1”?").arg(dstInfo.fileName()), QMessageBox::NoButton, this);
                box.setInformativeText(tr("Another folder with the same name already exists in “%1”.\nIf you replace "
                                          "the existing folder, all files in it will be lost.")
                                           .arg(dstInfo.absolutePath()));
                box.addButton(tr("&Cancel"), QMessageBox::RejectRole);
                QPushButton *replace = box.addButton(tr("&Replace"), QMessageBox::DestructiveRole);
                box.exec();
                if (box.clickedButton() != replace)
                    continue;
            }
            ok = copyTree(src, dst, &error);
            if (ok && path.isValid())
                recursivelyUpdate(path);
        }
        if (!ok) {
            QMessageBox::critical(this, tr("Error copying file"),
                                  tr("Couldn’t copy %1\nto %2.\n\n%3").arg(src, dst, error));
        }
    }
}

void DirDiff::deleteSelected()
{
    // Trash or delete all selected files/folders recursively
    const int pane = focusedPane();
    if (pane == -1)
        return;
    QList<QPersistentModelIndex> paths;
    for (TreeNode *node : selectedNodes(pane))
        paths.prepend(QPersistentModelIndex(model_->indexOf(node)));

    for (const QPersistentModelIndex &path : std::as_const(paths)) {
        if (!path.isValid())
            continue;
        const QString file = model_->valuePath(model_->node(path), pane);
        if (file.isEmpty() || !QFileInfo::exists(file))
            continue;
        QString error;
        const TrashResult result = trashOrConfirm(file, this, &error);
        if (result == TrashResult::Failed) {
            QMessageBox::critical(this, tr("Error deleting %1").arg(QDir::toNativeSeparators(file)), error);
            break;
        }
        if (result == TrashResult::Cancelled)
            break;
        if (path.isValid())
            fileDeleted(model_->node(path), pane);
    }
}

void DirDiff::actionOpenExternal()
{
    const int pane = focusedPane();
    if (pane == -1)
        return;
    QStringList files;
    for (TreeNode *node : selectedNodes(pane)) {
        const QString f = model_->valuePath(node, pane);
        if (!f.isEmpty())
            files.append(f);
    }
    openFilesExternal(files, this);
}

void DirDiff::actionCopyFilePaths()
{
    const int pane = focusedPane();
    if (pane == -1)
        return;
    QStringList files;
    for (TreeNode *node : selectedNodes(pane)) {
        const QString f = model_->valuePath(node, pane);
        if (!f.isEmpty())
            files.append(QDir::toNativeSeparators(f));
    }
    if (!files.isEmpty())
        QApplication::clipboard()->setText(files.join(QLatin1Char('\n')));
}

void DirDiff::actionFilterStateChange()
{
    const std::pair<int, std::pair<const char *, const char *>> stateActions[] = {
        {StateNormal, {"normal", "folder-status-same"}},
        {StateNoChange, {"normal", "folder-status-same"}},
        {StateNew, {"new", "folder-status-new"}},
        {StateModified, {"modified", "folder-status-modified"}},
    };
    std::vector<int> activeFilters;
    QStringList stateStrs;
    for (const auto &[state, names] : stateActions) {
        if (actionState(QLatin1String(names.second))) {
            activeFilters.push_back(state);
            stateStrs.append(QLatin1String(names.first));
        }
    }
    if (activeFilters == stateFilters_)
        return;
    stateFilters_ = activeFilters;
    stateStrs.removeDuplicates();
    MeldSettings::instance()->setValue(QStringLiteral("folder-status-filters"), stateStrs);
    refresh();
}

void DirDiff::syncVScroll(int master)
{
    if (doToOthersLock_)
        return;
    doToOthersLock_ = true;
    const int value = treeview_[size_t(master)]->verticalScrollBar()->value();
    for (int i = 0; i < numPanes_; ++i) {
        if (i != master)
            treeview_[size_t(i)]->verticalScrollBar()->setValue(value);
    }
    doToOthersLock_ = false;
}

void DirDiff::syncHScroll(int master)
{
    if (doToOthersLock_)
        return;
    doToOthersLock_ = true;
    const int value = treeview_[size_t(master)]->horizontalScrollBar()->value();
    for (int i = 0; i < numPanes_; ++i) {
        if (i != master)
            treeview_[size_t(i)]->horizontalScrollBar()->setValue(value);
    }
    doToOthersLock_ = false;
}

} // namespace meld
