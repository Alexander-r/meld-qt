#include "dirdifftree.h"

#include "core/iohelpers.h"
#include "icons.h"
#include "style.h"

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFont>
#include <QPalette>

namespace meld {

DirDiffTreeModel::DirDiffTreeModel(int ntree, QObject *parent)
    : QAbstractItemModel(parent)
    , ntree_(ntree)
    , root_(std::make_unique<TreeNode>())
{
    setupDefaultStyles();
}

DirDiffTreeModel::~DirDiffTreeModel() = default;

void DirDiffTreeModel::setupDefaultStyles()
{
    const Theme &theme = currentTheme();
    const QColor unk = theme.unknownForeground;
    const QColor nw = theme.insertForeground;
    const QColor mod = theme.replaceForeground;
    const QColor del = theme.deleteForeground;
    const QColor err = theme.errorForeground;
    const QColor con = theme.conflictForeground;
    const QColor none;

    textAttributes_ = {
        // foreground, italic, bold, strikethrough
        {unk, false, false, false}, // STATE_IGNORED
        {unk, false, false, false}, // STATE_NONE
        {none, false, false, false}, // STATE_NORMAL
        {none, true, false, false}, // STATE_NOCHANGE
        {err, false, true, false}, // STATE_ERROR
        {unk, true, false, false}, // STATE_EMPTY
        {nw, false, true, false}, // STATE_NEW
        {mod, false, true, false}, // STATE_MODIFIED
        {mod, false, false, false}, // STATE_RENAMED
        {con, false, true, false}, // STATE_CONFLICT
        {del, false, true, true}, // STATE_REMOVED
        {del, false, true, true}, // STATE_MISSING
        {unk, false, false, true}, // STATE_NONEXIST
        {none, true, false, false}, // STATE_SPINNER
    };

    const QString file = QStringLiteral("text-x-generic-symbolic");
    const QString folder = QStringLiteral("folder-symbolic");
    iconDetails_ = {
        // file-icon, folder-icon, file-tint
        {file, folder, none}, // IGNORED
        {file, folder, none}, // NONE
        {file, folder, none}, // NORMAL
        {file, folder, none}, // NOCHANGE
        {QStringLiteral("dialog-warning-symbolic"), QString(), none}, // ERROR
        {QString(), QString(), none}, // EMPTY
        {file, folder, nw}, // NEW
        {file, folder, mod}, // MODIFIED
        {file, folder, mod}, // RENAMED
        {file, folder, con}, // CONFLICT
        {file, folder, del}, // REMOVED
        {QString(), folder, unk}, // MISSING
        {QString(), folder, unk}, // NONEXIST
        {file, folder, none}, // SPINNER
    };
    Q_ASSERT(textAttributes_.size() == StateMax && iconDetails_.size() == StateMax);
}

QString DirDiffTreeModel::columnKey(Column column)
{
    switch (column) {
    case ColName:
        return QStringLiteral("name");
    case ColSize:
        return QStringLiteral("size");
    case ColModificationTime:
        return QStringLiteral("modification time");
    case ColModificationTimeIso:
        return QStringLiteral("iso-time");
    case ColPermissions:
        return QStringLiteral("permissions");
    case ColumnsPerPane:
        break;
    }
    return {};
}

TreeNode *DirDiffTreeModel::node(const QModelIndex &index) const
{
    if (!index.isValid())
        return root_.get();
    return static_cast<TreeNode *>(index.internalPointer());
}

QModelIndex DirDiffTreeModel::indexOf(const TreeNode *n, int column) const
{
    if (!n || n == root_.get())
        return {};
    return createIndex(n->row(), column, const_cast<TreeNode *>(n));
}

TreeNode *DirDiffTreeModel::firstRoot() const
{
    return root_->children.empty() ? nullptr : root_->children.front().get();
}

QModelIndex DirDiffTreeModel::index(int row, int column, const QModelIndex &parent) const
{
    const TreeNode *parentNode = node(parent);
    if (row < 0 || size_t(row) >= parentNode->children.size() || column < 0 || column >= columnCount())
        return {};
    return createIndex(row, column, parentNode->children[size_t(row)].get());
}

QModelIndex DirDiffTreeModel::parent(const QModelIndex &child) const
{
    if (!child.isValid())
        return {};
    const TreeNode *n = node(child);
    if (!n->parent || n->parent == root_.get())
        return {};
    return createIndex(n->parent->row(), 0, n->parent);
}

int DirDiffTreeModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid() && parent.column() != 0 && parent.column() % ColumnsPerPane != 0)
        return 0;
    return int(node(parent)->children.size());
}

bool DirDiffTreeModel::hasChildren(const QModelIndex &parent) const
{
    return !node(parent)->children.empty();
}

int DirDiffTreeModel::columnCount(const QModelIndex &) const
{
    return ntree_ * ColumnsPerPane;
}

Qt::ItemFlags DirDiffTreeModel::flags(const QModelIndex &index) const
{
    if (!index.isValid())
        return Qt::NoItemFlags;
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
}

QVariant DirDiffTreeModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (Column(section % ColumnsPerPane)) {
    case ColName:
        return tr("Name");
    case ColSize:
        return tr("Size");
    case ColModificationTime:
        return tr("Modification time");
    case ColModificationTimeIso:
        return tr("Modification time (ISO)");
    case ColPermissions:
        return tr("Permissions");
    case ColumnsPerPane:
        break;
    }
    return {};
}

QString DirDiffTreeModel::formatSize(qint64 size)
{
    if (size == -1)
        return {};
    static const char *suffixes[] = {"B", "kB", "MB", "GB", "TB", "PB", "EB", "ZB", "YB"};
    double value = double(size);
    int unit = 0;
    while (value > 1000 && unit < 8) {
        value /= 1000;
        ++unit;
    }
    if (unit > 0)
        return QStringLiteral("%1 %2").arg(value, 0, 'f', 1).arg(QLatin1String(suffixes[unit]));
    return QStringLiteral("%1 %2").arg(size).arg(QLatin1String(suffixes[unit]));
}

QString DirDiffTreeModel::formatTime(qint64 mtime)
{
    if (mtime <= MissingTimestamp)
        return {};
    return QDateTime::fromMSecsSinceEpoch(mtime).toString(QStringLiteral("ddd dd MMM yyyy HH:mm:ss"));
}

QString DirDiffTreeModel::formatIsoTime(qint64 mtime)
{
    if (mtime <= MissingTimestamp)
        return {};
    return QDateTime::fromMSecsSinceEpoch(mtime).toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss"));
}

QString DirDiffTreeModel::formatPermissions(int mode)
{
    if (mode == -1)
        return {};
    QString perms;
    static const std::pair<int, QChar> rwx[] = {{4, u'r'}, {2, u'w'}, {1, u'x'}};
    for (int groupIndex : {6, 3, 0}) {
        const int group = (mode >> groupIndex) & 7;
        for (const auto &[bit, letter] : rwx)
            perms += (group & bit) ? letter : QChar(u'-');
    }
    return perms;
}

QVariant DirDiffTreeModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid())
        return {};
    const TreeNode *n = node(index);
    const int pane = paneForColumn(index.column());
    if (pane >= ntree_)
        return {};
    const PaneEntry &entry = n->panes[size_t(pane)];
    const Column column = Column(index.column() % ColumnsPerPane);

    switch (role) {
    case PathRole:
        return entry.path;
    case StateRole:
        return entry.state;
    case Qt::ToolTipRole:
        return entry.path.isEmpty() ? QVariant() : QVariant(QDir::toNativeSeparators(entry.path));
    default:
        break;
    }

    if (column == ColName) {
        switch (role) {
        case Qt::DisplayRole:
            return entry.text;
        case Qt::DecorationRole: {
            if (entry.icon.isEmpty())
                return QVariant();
            const QColor color = entry.tint.isValid() ? entry.tint
                                                      : QApplication::palette().color(QPalette::Text);
            return icons::tintedPixmap(entry.icon, color, 16, qApp->devicePixelRatio());
        }
        case Qt::ForegroundRole:
            return entry.foreground.isValid() ? QVariant(entry.foreground) : QVariant();
        case Qt::FontRole: {
            if (!entry.italic && !entry.bold && !entry.strike)
                return QVariant();
            QFont font = QApplication::font();
            font.setItalic(entry.italic);
            font.setBold(entry.bold);
            font.setStrikeOut(entry.strike);
            return font;
        }
        default:
            return {};
        }
    }

    if (role != Qt::DisplayRole)
        return {};
    switch (column) {
    case ColSize:
        return formatSize(entry.size);
    case ColModificationTime:
        return formatTime(entry.mtime);
    case ColModificationTimeIso:
        return formatIsoTime(entry.mtime);
    case ColPermissions:
        return formatPermissions(entry.permissions);
    default:
        return {};
    }
}

void DirDiffTreeModel::clear()
{
    beginResetModel();
    root_->children.clear();
    endResetModel();
}

TreeNode *DirDiffTreeModel::addEntries(TreeNode *parentNode, const QStringList &paths)
{
    if (!parentNode)
        parentNode = root_.get();
    const int row = int(parentNode->children.size());
    beginInsertRows(indexOf(parentNode), row, row);
    auto child = std::make_unique<TreeNode>();
    child->parent = parentNode;
    child->rowIndex = row;
    for (int pane = 0; pane < ntree_ && pane < paths.size(); ++pane)
        child->panes[size_t(pane)].path = paths.at(pane);
    TreeNode *result = child.get();
    parentNode->children.push_back(std::move(child));
    endInsertRows();
    return result;
}

TreeNode *DirDiffTreeModel::addEmpty(TreeNode *parentNode, const QString &text)
{
    TreeNode *child = addEntries(parentNode, {});
    for (int pane = 0; pane < ntree_; ++pane)
        setState(child, pane, StateEmpty, text.isEmpty() ? tr("empty folder") : text);
    return child;
}

TreeNode *DirDiffTreeModel::addError(TreeNode *parentNode, const QString &message, int pane)
{
    TreeNode *child = addEntries(parentNode, {});
    for (int i = 0; i < ntree_; ++i) {
        PaneEntry &entry = child->panes[size_t(i)];
        entry.state = StateError;
        entry.mtime = MissingTimestamp;
        entry.size = -1;
        entry.permissions = -1;
    }
    setState(child, pane, StateError, message);
    return child;
}

void DirDiffTreeModel::removeNode(TreeNode *n)
{
    if (!n || !n->parent)
        return;
    TreeNode *parentNode = n->parent;
    const int row = n->row();
    beginRemoveRows(indexOf(parentNode), row, row);
    parentNode->children.erase(parentNode->children.begin() + row);
    for (size_t i = size_t(row); i < parentNode->children.size(); ++i)
        parentNode->children[i]->rowIndex = int(i);
    endRemoveRows();
}

void DirDiffTreeModel::removeChildren(TreeNode *n)
{
    if (!n || n->children.empty())
        return;
    beginRemoveRows(indexOf(n), 0, int(n->children.size()) - 1);
    n->children.clear();
    endRemoveRows();
}

QStringList DirDiffTreeModel::valuePaths(const TreeNode *n) const
{
    QStringList paths;
    for (int pane = 0; pane < ntree_; ++pane)
        paths.append(n->panes[size_t(pane)].path);
    return paths;
}

void DirDiffTreeModel::setPathState(TreeNode *n, int pane, int state, bool isDir, const QString &displayText)
{
    QString text = displayText;
    if (text.isEmpty())
        text = path::basename(n->panes[size_t(pane)].path);
    setState(n, pane, state, text, isDir);
}

void DirDiffTreeModel::setState(TreeNode *n, int pane, int state, const QString &label, bool isDir)
{
    PaneEntry &entry = n->panes[size_t(pane)];
    const IconDetails &icon = iconDetails_[size_t(state)];
    const TextAttributes &attrs = textAttributes_[size_t(state)];
    entry.state = state;
    entry.text = label;
    entry.isDir = isDir;
    entry.icon = isDir ? icon.folderIcon : icon.fileIcon;
    entry.tint = isDir ? QColor() : icon.fileTint;
    entry.foreground = attrs.foreground;
    entry.italic = attrs.italic;
    entry.bold = attrs.bold;
    entry.strike = attrs.strike;
    nodeChanged(n);
}

void DirDiffTreeModel::setStats(TreeNode *n, int pane, qint64 size, qint64 mtime, int permissions)
{
    PaneEntry &entry = n->panes[size_t(pane)];
    entry.size = size;
    entry.mtime = mtime;
    entry.permissions = permissions;
    nodeChanged(n);
}

void DirDiffTreeModel::setIcon(TreeNode *n, int pane, const QString &iconName)
{
    n->panes[size_t(pane)].icon = iconName;
    nodeChanged(n);
}

void DirDiffTreeModel::nodeChanged(TreeNode *n)
{
    const QModelIndex first = indexOf(n, 0);
    if (!first.isValid())
        return;
    Q_EMIT dataChanged(first, indexOf(n, columnCount() - 1));
}

TreeNode *DirDiffTreeModel::inorderNext(TreeNode *n) const
{
    if (!n)
        return nullptr;
    if (!n->children.empty())
        return n->children.front().get();
    while (n && n != root_.get()) {
        TreeNode *parentNode = n->parent;
        const int row = n->row();
        if (parentNode && size_t(row + 1) < parentNode->children.size())
            return parentNode->children[size_t(row + 1)].get();
        n = parentNode;
    }
    return nullptr;
}

TreeNode *DirDiffTreeModel::inorderPrevious(TreeNode *n) const
{
    if (!n || n == root_.get())
        return nullptr;
    const int row = n->row();
    if (row > 0) {
        TreeNode *prev = n->parent->children[size_t(row - 1)].get();
        while (!prev->children.empty())
            prev = prev->children.back().get();
        return prev;
    }
    return n->parent == root_.get() ? nullptr : n->parent;
}

std::pair<QModelIndex, QModelIndex>
DirDiffTreeModel::previousNextPaths(const QModelIndex &start, const std::function<bool(TreeNode *)> &match) const
{
    TreeNode *startNode = start.isValid() ? node(start) : nullptr;
    if (!startNode)
        return {};
    QModelIndex prev;
    QModelIndex next;
    for (TreeNode *n = inorderPrevious(startNode); n; n = inorderPrevious(n)) {
        if (match(n)) {
            prev = indexOf(n);
            break;
        }
    }
    for (TreeNode *n = inorderNext(startNode); n; n = inorderNext(n)) {
        if (match(n)) {
            next = indexOf(n);
            break;
        }
    }
    return {prev, next};
}

std::pair<QModelIndex, QModelIndex> DirDiffTreeModel::findNextPrevDiff(const QModelIndex &start) const
{
    // TODO: It works, but matching on the first pane only is very poor
    return previousNextPaths(start, [](TreeNode *n) {
        const int s = n->panes[0].state;
        return s != StateNormal && s != StateNoChange && s != StateEmpty;
    });
}

std::vector<TreeNode *> DirDiffTreeModel::stateRows(const std::vector<int> &states) const
{
    std::vector<TreeNode *> rows;
    for (TreeNode *n = firstRoot(); n; n = inorderNext(n)) {
        if (std::find(states.begin(), states.end(), n->panes[0].state) != states.end())
            rows.push_back(n);
    }
    return rows;
}

} // namespace meld
