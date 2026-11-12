#include "bufferselectors.h"

#include "core/syntax/language.h"

#include <QGridLayout>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListView>
#include <QSortFilterProxyModel>
#include <QStandardItemModel>

namespace meld {

namespace {

constexpr int ValueRole = Qt::UserRole + 1;

} // namespace

FilteredListSelector::FilteredListSelector(QWidget *parent)
    : QWidget(parent)
    , entry_(new QLineEdit(this))
    , list_(new QListView(this))
    , model_(new QStandardItemModel(this))
    , filter_(new QSortFilterProxyModel(this))
{
    setMinimumSize(300, 400);
    auto *layout = new QGridLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setVerticalSpacing(3);

    entry_->setClearButtonEnabled(true);
    layout->addWidget(entry_, 0, 0);

    filter_->setSourceModel(model_);
    filter_->setFilterCaseSensitivity(Qt::CaseInsensitive);
    list_->setModel(filter_);
    list_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    list_->setUniformItemSizes(true);
    layout->addWidget(list_, 1, 0);

    connect(entry_, &QLineEdit::textChanged, this, &FilteredListSelector::onEntryChanged);
    connect(entry_, &QLineEdit::returnPressed, this, &FilteredListSelector::onActivate);
    connect(list_, &QListView::activated, this, &FilteredListSelector::onActivate);
    // The selection can be moved from the entry
    entry_->installEventFilter(this);
}

void FilteredListSelector::addItem(const QString &name, const QVariant &value)
{
    auto *item = new QStandardItem(name);
    item->setData(value, ValueRole);
    model_->appendRow(item);
}

void FilteredListSelector::selectValue(const QVariant &value)
{
    if (value.isNull())
        return;
    for (int row = 0; row < filter_->rowCount(); ++row) {
        const QModelIndex index = filter_->index(row, 0);
        if (index.data(ValueRole) != value)
            continue;
        list_->setCurrentIndex(index);
        list_->scrollTo(index, QAbstractItemView::PositionAtCenter);
    }
}

QVariant FilteredListSelector::selectedValue() const
{
    const QModelIndexList selected = list_->selectionModel()->selectedIndexes();
    return selected.isEmpty() ? QVariant() : selected.constFirst().data(ValueRole);
}

void FilteredListSelector::onEntryChanged(const QString &text)
{
    filter_->setFilterFixedString(text);
    const QModelIndex first = filter_->index(0, 0);
    if (first.isValid())
        list_->setCurrentIndex(first);
}

void FilteredListSelector::onActivate()
{
    const QModelIndexList selected = list_->selectionModel()->selectedIndexes();
    if (selected.isEmpty())
        return;
    Q_EMIT valueSelected(selected.constFirst().data(ValueRole));
}

bool FilteredListSelector::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == entry_ && event->type() == QEvent::KeyPress) {
        const int key = static_cast<QKeyEvent *>(event)->key();
        if (key == Qt::Key_Up || key == Qt::Key_Down || key == Qt::Key_PageUp || key == Qt::Key_PageDown) {
            QKeyEvent copy(QEvent::KeyPress, key, static_cast<QKeyEvent *>(event)->modifiers());
            QCoreApplication::sendEvent(list_, &copy);
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

SourceLangSelector::SourceLangSelector(QWidget *parent)
    : FilteredListSelector(parent)
{
    entry()->setPlaceholderText(tr("Search highlight mode…"));
    addItem(valueLabel(QString()), QVariant());
    const syntax::LanguageManager &manager = syntax::LanguageManager::instance();
    for (const QString &id : manager.languageIds())
        addItem(manager.language(id)->name(), id);
}

QString SourceLangSelector::valueLabel(const QString &languageId)
{
    if (const syntax::Language *language = syntax::LanguageManager::instance().language(languageId))
        return language->name();
    return tr("Plain Text");
}

} // namespace meld
