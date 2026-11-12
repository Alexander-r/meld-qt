#pragma once

#include <QVariant>
#include <QWidget>

class QLineEdit;
class QListView;
class QSortFilterProxyModel;
class QStandardItemModel;

namespace meld {

/*!
 * A list of values with names, filtered by a search entry
 * (FilteredListSelector, initially based on gedit's
 * GeditHighlightModeSelector)
 */
class FilteredListSelector : public QWidget {
    Q_OBJECT

public:
    explicit FilteredListSelector(QWidget *parent = nullptr);

    void addItem(const QString &name, const QVariant &value);
    //! Select and show the row with this value; null values aren't selected
    void selectValue(const QVariant &value);
    QVariant selectedValue() const;

    QLineEdit *entry() const { return entry_; }
    QListView *list() const { return list_; }

Q_SIGNALS:
    //! A value was chosen, by activating its row or the entry
    void valueSelected(const QVariant &value);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void onEntryChanged(const QString &text);
    void onActivate();

    QLineEdit *entry_;
    QListView *list_;
    QStandardItemModel *model_;
    QSortFilterProxyModel *filter_;
};

//! Selects a highlighting language, by id, or plain text (SourceLangSelector)
class SourceLangSelector : public FilteredListSelector {
    Q_OBJECT

public:
    explicit SourceLangSelector(QWidget *parent = nullptr);

    //! The label of a language id; an empty id is plain text
    static QString valueLabel(const QString &languageId);
};

} // namespace meld
