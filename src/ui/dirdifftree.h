#pragma once

#include "core/vc/states.h"

#include <QAbstractItemModel>
#include <QColor>
#include <QStringList>

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace meld {

//! Sentinel value for mtimes on files that don't exist.
constexpr qint64 MissingTimestamp = -2147483648LL;

struct PaneEntry {
    QString path;
    int state = -1;
    QString text;
    QString icon;
    QColor tint;
    QColor foreground;
    bool italic = false;
    bool bold = false;
    bool strike = false;
    bool isDir = false;
    qint64 size = -1;
    qint64 mtime = MissingTimestamp;
    int permissions = -1;
};

struct TreeNode {
    TreeNode *parent = nullptr;
    std::vector<std::unique_ptr<TreeNode>> children;
    std::array<PaneEntry, 3> panes;
    //! Extra text columns, used by version control views
    std::array<QString, 3> extra;
    //! Index of this node within its parent's children
    int rowIndex = 0;

    int row() const { return rowIndex; }
};

/*!
 * Tree model with one set of columns per comparison pane. Each tree view in
 * the folder comparison shows only its own pane's columns.
 */
class DirDiffTreeModel : public QAbstractItemModel {
    Q_OBJECT

public:
    enum Column {
        ColName,
        ColSize,
        ColModificationTime,
        ColModificationTimeIso,
        ColPermissions,
        ColumnsPerPane,
    };

    enum Roles {
        PathRole = Qt::UserRole + 1,
        StateRole,
    };

    explicit DirDiffTreeModel(int ntree, QObject *parent = nullptr);
    ~DirDiffTreeModel() override;

    int ntree() const { return ntree_; }
    static int columnIndex(Column column, int pane) { return pane * ColumnsPerPane + column; }
    static int paneForColumn(int column) { return column / ColumnsPerPane; }
    static QString columnKey(Column column);

    // QAbstractItemModel
    QModelIndex index(int row, int column, const QModelIndex &parent = QModelIndex()) const override;
    QModelIndex parent(const QModelIndex &child) const override;
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    bool hasChildren(const QModelIndex &parent = QModelIndex()) const override;

    TreeNode *node(const QModelIndex &index) const;
    QModelIndex indexOf(const TreeNode *node, int column = 0) const;
    TreeNode *invisibleRoot() const { return root_.get(); }
    TreeNode *firstRoot() const;

    void clear();
    TreeNode *addEntries(TreeNode *parent, const QStringList &paths);
    TreeNode *addEmpty(TreeNode *parent, const QString &text = QString());
    TreeNode *addError(TreeNode *parent, const QString &message, int pane);
    void removeNode(TreeNode *node);
    void removeChildren(TreeNode *node);

    QStringList valuePaths(const TreeNode *node) const;
    QString valuePath(const TreeNode *node, int pane) const { return node->panes[size_t(pane)].path; }
    int state(const TreeNode *node, int pane) const { return node->panes[size_t(pane)].state; }

    void setPathState(TreeNode *node, int pane, int state, bool isDir, const QString &displayText = QString());
    void setState(TreeNode *node, int pane, int state, const QString &label, bool isDir = false);
    void setStats(TreeNode *node, int pane, qint64 size, qint64 mtime, int permissions);
    void setIcon(TreeNode *node, int pane, const QString &icon);
    void nodeChanged(TreeNode *node);

    //! Depth-first traversal, as SearchableTreeStore.inorder_search_*
    TreeNode *inorderNext(TreeNode *node) const;
    TreeNode *inorderPrevious(TreeNode *node) const;
    std::pair<QModelIndex, QModelIndex> previousNextPaths(const QModelIndex &start,
                                                          const std::function<bool(TreeNode *)> &match) const;
    //! Rows whose first pane is not normal, previous and next of start
    std::pair<QModelIndex, QModelIndex> findNextPrevDiff(const QModelIndex &start) const;
    std::vector<TreeNode *> stateRows(const std::vector<int> &states) const;

    //! Formatting helpers matching the original cell renderers
    static QString formatSize(qint64 size);
    static QString formatTime(qint64 mtime);
    static QString formatIsoTime(qint64 mtime);
    static QString formatPermissions(int mode);

private:
    struct TextAttributes {
        QColor foreground;
        bool italic;
        bool bold;
        bool strike;
    };
    struct IconDetails {
        QString fileIcon;
        QString folderIcon;
        QColor fileTint;
    };
    void setupDefaultStyles();

    int ntree_;
    std::unique_ptr<TreeNode> root_;
    std::vector<TextAttributes> textAttributes_;
    std::vector<IconDetails> iconDetails_;
};

} // namespace meld
