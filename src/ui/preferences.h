#pragma once

#include "core/filters.h"

#include <QDialog>
#include <QWidget>

class QTreeWidget;
class QToolButton;

namespace meld {

//! Editable list of (name, active, pattern) filters stored in a settings key
class FilterListWidget : public QWidget {
    Q_OBJECT

public:
    FilterListWidget(const QString &settingsKey, FilterEntry::Type type, QWidget *parent = nullptr);

private:
    void load();
    void save();
    void updateSensitivity();
    void addEntry();
    void removeEntry();
    void moveEntry(int offset);

    QString key_;
    FilterEntry::Type type_;
    QTreeWidget *tree_;
    QToolButton *remove_;
    QToolButton *up_;
    QToolButton *down_;
    bool loading_ = false;
};

class PreferencesDialog : public QDialog {
    Q_OBJECT

public:
    explicit PreferencesDialog(QWidget *parent = nullptr);

private:
    QWidget *buildEditorPage();
    QWidget *buildFolderPage();
    QWidget *buildVersionControlPage();
    QWidget *buildFiltersPage();
};

} // namespace meld
