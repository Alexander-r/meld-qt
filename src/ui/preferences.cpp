#include "preferences.h"

#include "core/syntax/highlighter.h"
#include "core/syntax/language.h"
#include "core/syntax/stylescheme.h"
#include "core/translations.h"
#include "icons.h"
#include "settings.h"
#include "style.h"

#include <QCheckBox>
#include <QCollator>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFontDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QTabWidget>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace meld {

namespace {

QCheckBox *boolSetting(const QString &label, const QString &key, QWidget *parent)
{
    auto *check = new QCheckBox(label, parent);
    check->setChecked(MeldSettings::instance()->boolValue(key));
    QObject::connect(check, &QCheckBox::toggled, check,
                     [key](bool checked) { MeldSettings::instance()->setValue(key, checked); });
    return check;
}

QGroupBox *group(const QString &title, QWidget *parent)
{
    auto *box = new QGroupBox(title, parent);
    new QVBoxLayout(box);
    return box;
}

//! Add a nested layout to a group; unlike QLayout::addItem(), this also
//! reparents the widgets the nested layout created (e.g., form labels)
void addGroupLayout(QGroupBox *box, QLayout *layout)
{
    static_cast<QBoxLayout *>(box->layout())->addLayout(layout);
}

enum Columns {
    ColName,
    ColActive,
    ColPattern,
};

//! A preview of a scheme, as GtkSourceStyleSchemeChooserWidget shows them
QWidget *schemePreview(const syntax::StyleScheme *scheme, QWidget *parent)
{
    auto *row = new QWidget(parent);
    auto *layout = new QVBoxLayout(row);
    layout->setContentsMargins(6, 6, 6, 6);
    auto *view = new QPlainTextEdit(row);
    view->setReadOnly(true);
    view->setFocusPolicy(Qt::NoFocus);
    view->setAttribute(Qt::WA_TransparentForMouseEvents);
    view->setLineWrapMode(QPlainTextEdit::NoWrap);
    view->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    view->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    view->setPalette(schemeTextPalette(scheme));
    const QFont font = editorFont();
    view->setFont(font);
    view->document()->setDefaultFont(font);

    auto *highlighter = new syntax::SyntaxHighlighter(view->document());
    highlighter->setBaseFont(font);
    highlighter->setStyleScheme(scheme);
    highlighter->setLanguage(syntax::LanguageManager::instance().language(QStringLiteral("c")));
    view->setPlainText(QStringLiteral("/* %1 */\n#define X 1\nstatic char *x = \"%2\"")
                           .arg(scheme->name(), scheme->description()));
    highlighter->highlightAll();

    const QFontMetricsF metrics(font);
    view->setFixedHeight(int(metrics.lineSpacing() * 3 + view->document()->documentMargin() * 2
                             + view->frameWidth() * 2 + 2));
    layout->addWidget(view);
    return row;
}

/*!
 * Choose a style scheme, as GtkSourceStyleSchemeChooserButton's dialog does
 *
 * Returns the chosen scheme's id, or nullopt if the dialog was cancelled.
 */
std::optional<QString> chooseStyleScheme(QWidget *parent, const QString &current)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(QObject::tr("Select a Style"));
    dialog.resize(480, 560);
    auto *layout = new QVBoxLayout(&dialog);
    auto *list = new QListWidget(&dialog);
    list->setSelectionMode(QAbstractItemView::SingleSelection);
    list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    const syntax::StyleSchemeManager &manager = syntax::StyleSchemeManager::instance();
    for (const QString &id : manager.schemeIds()) {
        const syntax::StyleScheme *scheme = manager.scheme(id);
        auto *item = new QListWidgetItem(list);
        item->setData(Qt::UserRole, id);
        item->setData(Qt::AccessibleTextRole, scheme->name());
        QWidget *preview = schemePreview(scheme, list);
        item->setSizeHint(preview->sizeHint());
        list->setItemWidget(item, preview);
        if (id == current)
            list->setCurrentItem(item);
    }
    layout->addWidget(list);

    auto *buttons = new QDialogButtonBox(&dialog);
    buttons->addButton(QDialogButtonBox::Cancel);
    QPushButton *select = buttons->addButton(QObject::tr("Select"), QDialogButtonBox::AcceptRole);
    select->setDefault(true);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(list, &QListWidget::itemActivated, &dialog, &QDialog::accept);
    layout->addWidget(buttons);

    if (QListWidgetItem *item = list->currentItem())
        list->scrollToItem(item);
    if (dialog.exec() != QDialog::Accepted || !list->currentItem())
        return std::nullopt;
    return list->currentItem()->data(Qt::UserRole).toString();
}

QString schemeName(const QString &id)
{
    const syntax::StyleScheme *scheme = syntax::StyleSchemeManager::instance().scheme(id);
    return scheme ? scheme->name() : id;
}

} // namespace

// ---------------------------------------------------------------------------
// FilterListWidget

FilterListWidget::FilterListWidget(const QString &settingsKey, FilterEntry::Type type, QWidget *parent)
    : QWidget(parent)
    , key_(settingsKey)
    , type_(type)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    tree_ = new QTreeWidget(this);
    tree_->setColumnCount(3);
    tree_->setHeaderLabels({tr("Name"), tr("Active"), tr("Pattern")});
    tree_->setRootIsDecorated(false);
    tree_->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    tree_->header()->setSectionResizeMode(ColName, QHeaderView::ResizeToContents);
    tree_->header()->setSectionResizeMode(ColActive, QHeaderView::ResizeToContents);
    tree_->header()->setStretchLastSection(true);
    tree_->setMinimumHeight(180);
    layout->addWidget(tree_);

    auto *toolbar = new QHBoxLayout;
    toolbar->setContentsMargins(0, 2, 0, 0);
    toolbar->setSpacing(0);
    auto *add = new QToolButton(this);
    add->setText(QStringLiteral("+"));
    add->setToolTip(tr("Add new filter"));
    remove_ = new QToolButton(this);
    remove_->setText(QStringLiteral("−"));
    remove_->setToolTip(tr("Remove selected filter"));
    up_ = new QToolButton(this);
    up_->setIcon(icons::icon(QStringLiteral("go-up-symbolic")));
    up_->setToolTip(tr("Move item up"));
    down_ = new QToolButton(this);
    down_->setIcon(icons::icon(QStringLiteral("go-down-symbolic")));
    down_->setToolTip(tr("Move item down"));
    for (QToolButton *button : {add, remove_, up_, down_})
        toolbar->addWidget(button);
    toolbar->addStretch(1);
    layout->addLayout(toolbar);

    connect(add, &QToolButton::clicked, this, &FilterListWidget::addEntry);
    connect(remove_, &QToolButton::clicked, this, &FilterListWidget::removeEntry);
    connect(up_, &QToolButton::clicked, this, [this]() { moveEntry(-1); });
    connect(down_, &QToolButton::clicked, this, [this]() { moveEntry(1); });
    connect(tree_, &QTreeWidget::itemSelectionChanged, this, &FilterListWidget::updateSensitivity);
    connect(tree_, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem *item, int column) {
        if (loading_)
            return;
        if (column == ColPattern) {
            const bool valid = FilterEntry::checkFilter(item->text(ColPattern), type_);
            loading_ = true;
            item->setIcon(ColPattern, valid ? QIcon() : icons::icon(QStringLiteral("dialog-warning-symbolic")));
            loading_ = false;
        }
        save();
    });

    load();
    updateSensitivity();
}

void FilterListWidget::load()
{
    loading_ = true;
    tree_->clear();
    for (const FilterSetting &filter : MeldSettings::instance()->filterSettings(key_)) {
        auto *item = new QTreeWidgetItem(tree_);
        item->setFlags(item->flags() | Qt::ItemIsEditable | Qt::ItemIsUserCheckable);
        item->setText(ColName, filter.name);
        item->setCheckState(ColActive, filter.active ? Qt::Checked : Qt::Unchecked);
        item->setText(ColPattern, filter.pattern);
        if (!FilterEntry::checkFilter(filter.pattern, type_))
            item->setIcon(ColPattern, icons::icon(QStringLiteral("dialog-warning-symbolic")));
    }
    loading_ = false;
}

void FilterListWidget::save()
{
    QList<FilterSetting> filters;
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
        const QTreeWidgetItem *item = tree_->topLevelItem(i);
        filters.append({item->text(ColName), item->checkState(ColActive) == Qt::Checked, item->text(ColPattern)});
    }
    MeldSettings::instance()->setFilterSettings(key_, filters);
}

void FilterListWidget::updateSensitivity()
{
    const QTreeWidgetItem *item = tree_->currentItem();
    const bool haveSelection = item && item->isSelected();
    const int index = item ? tree_->indexOfTopLevelItem(const_cast<QTreeWidgetItem *>(item)) : -1;
    remove_->setEnabled(haveSelection);
    up_->setEnabled(haveSelection && index > 0);
    down_->setEnabled(haveSelection && index < tree_->topLevelItemCount() - 1);
}

void FilterListWidget::addEntry()
{
    loading_ = true;
    auto *item = new QTreeWidgetItem(tree_);
    item->setFlags(item->flags() | Qt::ItemIsEditable | Qt::ItemIsUserCheckable);
    item->setText(ColName, tr("label"));
    item->setCheckState(ColActive, Qt::Unchecked);
    item->setText(ColPattern, tr("pattern"));
    loading_ = false;
    save();
    tree_->setCurrentItem(item);
    tree_->editItem(item, ColName);
}

void FilterListWidget::removeEntry()
{
    delete tree_->currentItem();
    save();
    updateSensitivity();
}

void FilterListWidget::moveEntry(int offset)
{
    QTreeWidgetItem *item = tree_->currentItem();
    if (!item)
        return;
    const int index = tree_->indexOfTopLevelItem(item);
    const int target = index + offset;
    if (target < 0 || target >= tree_->topLevelItemCount())
        return;
    loading_ = true;
    tree_->takeTopLevelItem(index);
    tree_->insertTopLevelItem(target, item);
    loading_ = false;
    tree_->setCurrentItem(item);
    save();
    updateSensitivity();
}

// ---------------------------------------------------------------------------
// PreferencesDialog

PreferencesDialog::PreferencesDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Preferences"));
    resize(640, 640);
    auto *layout = new QVBoxLayout(this);
    auto *tabs = new QTabWidget(this);
    tabs->setDocumentMode(true);

    const auto scrolled = [this](QWidget *page) {
        auto *area = new QScrollArea(this);
        area->setWidgetResizable(true);
        area->setFrameShape(QFrame::NoFrame);
        area->setWidget(page);
        return area;
    };
    tabs->addTab(scrolled(buildEditorPage()), tr("Editor"));
    tabs->addTab(scrolled(buildFolderPage()), tr("Folder Comparison"));
    tabs->addTab(scrolled(buildVersionControlPage()), tr("Version Control"));
    tabs->addTab(scrolled(buildFiltersPage()), tr("Filters"));
    layout->addWidget(tabs);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

QWidget *PreferencesDialog::buildEditorPage()
{
    MeldSettings *settings = MeldSettings::instance();
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);

    // Appearance
    QGroupBox *appearance = group(tr("Appearance"), page);
    auto *styleRow = new QFormLayout;
    auto *style = new QComboBox(appearance);
    style->addItem(tr("Follow System Style"), QStringLiteral("default"));
    style->addItem(tr("Light"), QStringLiteral("force-light"));
    style->addItem(tr("Dark"), QStringLiteral("force-dark"));
    style->setCurrentIndex(std::max(0, style->findData(settings->stringValue(QStringLiteral("style-variant")))));
    connect(style, &QComboBox::currentIndexChanged, this, [style]() {
        MeldSettings::instance()->setValue(QStringLiteral("style-variant"), style->currentData());
    });
    styleRow->addRow(tr("Interface Style"), style);

    // Not in the original, which uses the system's language
    auto *language = new QComboBox(appearance);
    language->setObjectName(QStringLiteral("language-chooser"));
    language->addItem(tr("System Language"), QString());
    // The languages by their own names, as the user's language sorts them
    QList<std::pair<QString, QString>> languages;
    for (const QString &name : translations::available())
        languages.append({translations::nativeName(name), name});
    QCollator collator;
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    std::ranges::sort(languages, [&collator](const auto &a, const auto &b) { return collator(a.first, b.first); });
    for (const auto &[label, name] : std::as_const(languages))
        language->addItem(label, name);
    language->setCurrentIndex(std::max(0, language->findData(settings->stringValue(QStringLiteral("language")))));
    styleRow->addRow(tr("Language"), language);
    // The windows' text isn't changed in place, so the language is changed
    // when the program is next started
    auto *restart = new QLabel(tr("The language will change when Meld Qt is next started."), appearance);
    restart->setWordWrap(true);
    styleRow->addRow(static_cast<QWidget *>(nullptr), restart);
    const auto updateRestart = [styleRow, language, restart]() {
        styleRow->setRowVisible(restart, translations::select(language->currentData().toString())
                                             != translations::current());
    };
    updateRestart();
    connect(language, &QComboBox::currentIndexChanged, this, [language, updateRestart]() {
        MeldSettings::instance()->setValue(QStringLiteral("language"), language->currentData());
        updateRestart();
    });
    addGroupLayout(appearance, styleRow);

    auto *customFont = new QCheckBox(tr("Use Custom Font"), appearance);
    customFont->setChecked(!settings->boolValue(QStringLiteral("use-system-font")));
    auto *fontButton = new QPushButton(appearance);
    const auto updateFontButton = [fontButton]() {
        const QFont font = MeldSettings::instance()->parseFontDescription(
            MeldSettings::instance()->stringValue(QStringLiteral("custom-font")));
        fontButton->setText(QStringLiteral("%1 %2").arg(font.family()).arg(font.pointSizeF()));
    };
    updateFontButton();
    fontButton->setEnabled(customFont->isChecked());
    connect(customFont, &QCheckBox::toggled, this, [fontButton](bool checked) {
        fontButton->setEnabled(checked);
        MeldSettings::instance()->setValue(QStringLiteral("use-system-font"), !checked);
    });
    connect(fontButton, &QPushButton::clicked, this, [this, updateFontButton]() {
        bool ok = false;
        const QFont current = MeldSettings::parseFontDescription(
            MeldSettings::instance()->stringValue(QStringLiteral("custom-font")));
        const QFont font = QFontDialog::getFont(&ok, current, this, tr("Font"), QFontDialog::MonospacedFonts);
        if (ok) {
            MeldSettings::instance()->setValue(QStringLiteral("custom-font"), MeldSettings::fontDescription(font));
            updateFontButton();
        }
    });
    auto *fontRow = new QHBoxLayout;
    fontRow->addWidget(customFont);
    fontRow->addStretch(1);
    fontRow->addWidget(fontButton);
    addGroupLayout(appearance, fontRow);

    QCheckBox *syntax = boolSetting(tr("Use Syntax Highlighting"), QStringLiteral("highlight-syntax"), appearance);
    auto *schemeLabel = new QLabel(tr("Color Scheme"), appearance);
    auto *schemeButton = new QPushButton(appearance);
    schemeButton->setObjectName(QStringLiteral("style-scheme-chooser"));
    const auto updateSchemeButton = [schemeButton]() {
        schemeButton->setText(schemeName(MeldSettings::instance()->stringValue(QStringLiteral("style-scheme"))));
    };
    updateSchemeButton();
    const auto updateSchemeSensitivity = [schemeLabel, schemeButton](bool enabled) {
        schemeLabel->setEnabled(enabled);
        schemeButton->setEnabled(enabled);
    };
    updateSchemeSensitivity(syntax->isChecked());
    connect(syntax, &QCheckBox::toggled, this, updateSchemeSensitivity);
    connect(schemeButton, &QPushButton::clicked, this, [this]() {
        MeldSettings *settings = MeldSettings::instance();
        if (const std::optional<QString> id =
                chooseStyleScheme(this, settings->stringValue(QStringLiteral("style-scheme"))))
            settings->setValue(QStringLiteral("style-scheme"), *id);
    });
    connect(settings, &MeldSettings::changed, schemeButton, [updateSchemeButton](const QString &key) {
        if (key == QLatin1String("style-scheme"))
            updateSchemeButton();
    });
    appearance->layout()->addWidget(syntax);
    auto *schemeRow = new QHBoxLayout;
    schemeRow->addSpacing(24);
    schemeRow->addWidget(schemeLabel);
    schemeRow->addStretch(1);
    schemeRow->addWidget(schemeButton);
    addGroupLayout(appearance, schemeRow);
    layout->addWidget(appearance);

    // Wrapping
    QGroupBox *display = group(tr("Display"), page);
    auto *wrapRow = new QFormLayout;
    auto *wrap = new QComboBox(display);
    wrap->addItem(tr("Never"), QStringLiteral("none"));
    wrap->addItem(tr("At Any Character"), QStringLiteral("char"));
    wrap->addItem(tr("At Word Boundaries"), QStringLiteral("word"));
    wrap->setCurrentIndex(std::max(0, wrap->findData(settings->stringValue(QStringLiteral("wrap-mode")))));
    connect(wrap, &QComboBox::currentIndexChanged, this,
            [wrap]() { MeldSettings::instance()->setValue(QStringLiteral("wrap-mode"), wrap->currentData()); });
    wrapRow->addRow(tr("Wrap Text"), wrap);
    addGroupLayout(display, wrapRow);
    display->layout()->addWidget(
        boolSetting(tr("Highlight Current Line"), QStringLiteral("highlight-current-line"), display));
    display->layout()->addWidget(boolSetting(tr("Show Line Numbers"), QStringLiteral("show-line-numbers"), display));
    display->layout()->addWidget(boolSetting(tr("Show whitespace"), QStringLiteral("enable-space-drawer"), display));
    layout->addWidget(display);

    // Indentation
    QGroupBox *indentation = group(tr("Indentation"), page);
    auto *indentForm = new QFormLayout;
    auto *tabCharacter = new QComboBox(indentation);
    tabCharacter->addItem(tr("Tabs"), false);
    tabCharacter->addItem(tr("Spaces"), true);
    tabCharacter->setCurrentIndex(settings->boolValue(QStringLiteral("insert-spaces-instead-of-tabs")) ? 1 : 0);
    connect(tabCharacter, &QComboBox::currentIndexChanged, this, [tabCharacter]() {
        MeldSettings::instance()->setValue(QStringLiteral("insert-spaces-instead-of-tabs"), tabCharacter->currentData());
    });
    indentForm->addRow(tr("Tab character"), tabCharacter);
    auto *tabSize = new QSpinBox(indentation);
    tabSize->setRange(1, 32);
    tabSize->setValue(settings->intValue(QStringLiteral("indent-width")));
    connect(tabSize, &QSpinBox::valueChanged, this,
            [](int value) { MeldSettings::instance()->setValue(QStringLiteral("indent-width"), value); });
    indentForm->addRow(tr("&Indentation Size"), tabSize);
    addGroupLayout(indentation, indentForm);
    layout->addWidget(indentation);

    QGroupBox *overview = group(tr("Code Overview"), page);
    overview->layout()->addWidget(boolSetting(tr("Show overview map"), QStringLiteral("show-overview-map"), overview));
    layout->addWidget(overview);

    // External editor
    QGroupBox *editor = group(tr("External Editor"), page);
    auto *customEditor = new QCheckBox(tr("Use custom text editor"), editor);
    customEditor->setChecked(!settings->boolValue(QStringLiteral("use-system-editor")));
    auto *command = new QLineEdit(settings->stringValue(QStringLiteral("custom-editor-command")), editor);
    command->setPlaceholderText(tr("Custom Editor Command"));
    command->setToolTip(tr("Use {file} and {line} to refer to the file and line being opened"));
    command->setEnabled(customEditor->isChecked());
    connect(customEditor, &QCheckBox::toggled, this, [command](bool checked) {
        command->setEnabled(checked);
        MeldSettings::instance()->setValue(QStringLiteral("use-system-editor"), !checked);
    });
    connect(command, &QLineEdit::textChanged, this,
            [](const QString &text) { MeldSettings::instance()->setValue(QStringLiteral("custom-editor-command"), text); });
    editor->layout()->addWidget(customEditor);
    editor->layout()->addWidget(command);
    layout->addWidget(editor);

#ifdef MELD_SINGLE_INSTANCE
    // Not in the original, which is always a single instance
    QGroupBox *application = group(tr("Application"), page);
    QCheckBox *singleInstance =
        boolSetting(tr("Open comparisons in the running Meld Qt"), QStringLiteral("single-instance"), application);
    singleInstance->setToolTip(tr("Comparisons opened from the command line or other programs open in a new window "
                                  "of a Meld Qt that's already running, instead of starting another Meld Qt. "
                                  "This applies to Meld Qt started after the change."));
    application->layout()->addWidget(singleInstance);
    layout->addWidget(application);
#endif
    layout->addStretch(1);
    return page;
}

QWidget *PreferencesDialog::buildFolderPage()
{
    MeldSettings *settings = MeldSettings::instance();
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);

    QGroupBox *options = group(tr("Comparison Options"), page);
    auto *shallow = boolSetting(tr("Compare files based only on size and timestamp"),
                                QStringLiteral("folder-shallow-comparison"), options);
    options->layout()->addWidget(shallow);
    auto *resolutionForm = new QFormLayout;
    auto *resolution = new QComboBox(options);
    const std::pair<QString, int> resolutions[] = {
        {tr("1ns (ext4)"), 1},      {tr("100ns (NTFS)"), 100},          {tr("1s (ext2/ext3)"), 1000000000},
        {tr("2s (VFAT)"), 2000000000}, {tr("Ignore timestamp"), -1},
    };
    for (const auto &[label, value] : resolutions)
        resolution->addItem(label, value);
    resolution->setCurrentIndex(std::max(0, resolution->findData(settings->intValue(QStringLiteral("folder-time-resolution")))));
    resolution->setEnabled(shallow->isChecked());
    connect(shallow, &QCheckBox::toggled, resolution, &QComboBox::setEnabled);
    connect(resolution, &QComboBox::currentIndexChanged, this, [resolution]() {
        MeldSettings::instance()->setValue(QStringLiteral("folder-time-resolution"), resolution->currentData());
    });
    resolutionForm->addRow(tr("Timestamp resolution"), resolution);
    addGroupLayout(options, resolutionForm);
    options->layout()->addWidget(
        boolSetting(tr("Ignore symbolic links"), QStringLiteral("folder-ignore-symlinks"), options));
    options->layout()->addWidget(boolSetting(tr("Apply text filters during folder comparisons"),
                                             QStringLiteral("folder-filter-text"), options));
    layout->addWidget(options);

    QGroupBox *columnsBox = group(tr("Visible Columns"), page);
    auto *columns = new QListWidget(columnsBox);
    columns->setDragDropMode(QAbstractItemView::InternalMove);
    const std::pair<QString, QString> columnLabels[] = {
        {QStringLiteral("size"), tr("Size")},
        {QStringLiteral("modification time"), tr("Modification time")},
        {QStringLiteral("iso-time"), tr("Modification time (ISO)")},
        {QStringLiteral("permissions"), tr("Permissions")},
    };
    QList<ColumnSetting> stored = settings->folderColumns();
    for (const auto &[key, label] : columnLabels) {
        bool found = false;
        for (const ColumnSetting &c : std::as_const(stored))
            found = found || c.name == key;
        if (!found)
            stored.append({key, false});
    }
    for (const ColumnSetting &column : std::as_const(stored)) {
        QString label = column.name;
        for (const auto &[key, text] : columnLabels) {
            if (key == column.name)
                label = text;
        }
        auto *item = new QListWidgetItem(label, columns);
        item->setData(Qt::UserRole, column.name);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsDragEnabled);
        item->setCheckState(column.visible ? Qt::Checked : Qt::Unchecked);
    }
    const auto saveColumns = [columns]() {
        QList<ColumnSetting> result;
        for (int i = 0; i < columns->count(); ++i) {
            const QListWidgetItem *item = columns->item(i);
            result.append({item->data(Qt::UserRole).toString(), item->checkState() == Qt::Checked});
        }
        MeldSettings::instance()->setFolderColumns(result);
    };
    connect(columns, &QListWidget::itemChanged, this, saveColumns);
    connect(columns->model(), &QAbstractItemModel::rowsMoved, this, saveColumns);
    columns->setMaximumHeight(130);
    columnsBox->layout()->addWidget(columns);
    layout->addWidget(columnsBox);
    layout->addStretch(1);
    return page;
}

QWidget *PreferencesDialog::buildVersionControlPage()
{
    MeldSettings *settings = MeldSettings::instance();
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);

    QGroupBox *comparisons = group(tr("Version Comparisons"), page);
    auto *form = new QFormLayout;
    auto *versionOrder = new QComboBox(comparisons);
    versionOrder->addItem(tr("Left is remote, right is local"), false);
    versionOrder->addItem(tr("Left is local, right is remote"), true);
    versionOrder->setCurrentIndex(settings->boolValue(QStringLiteral("vc-left-is-local")) ? 1 : 0);
    connect(versionOrder, &QComboBox::currentIndexChanged, this, [versionOrder]() {
        MeldSettings::instance()->setValue(QStringLiteral("vc-left-is-local"), versionOrder->currentData());
    });
    form->addRow(tr("Order when comparing file revisions"), versionOrder);
    auto *mergeOrder = new QComboBox(comparisons);
    mergeOrder->addItem(tr("Remote, merge, local"), QStringLiteral("remote-merge-local"));
    mergeOrder->addItem(tr("Local, merge, remote"), QStringLiteral("local-merge-remote"));
    mergeOrder->setCurrentIndex(std::max(0, mergeOrder->findData(settings->stringValue(QStringLiteral("vc-merge-file-order")))));
    connect(mergeOrder, &QComboBox::currentIndexChanged, this, [mergeOrder]() {
        MeldSettings::instance()->setValue(QStringLiteral("vc-merge-file-order"), mergeOrder->currentData());
    });
    form->addRow(tr("Order when merging files"), mergeOrder);
    addGroupLayout(comparisons, form);
    // Files with conflict markers are merged with conflicts empty, or with
    // the common ancestor's text if the markers have it (diff3 style)
    auto *ancestor = boolSetting(tr("Show the common ancestor in conflicts from conflict markers"),
                                 QStringLiteral("conflict-show-ancestor"), comparisons);
    ancestor->setToolTip(tr("Merges of files with diff3-style conflict markers start with the common ancestor’s "
                            "text in conflicts, rather than nothing"));
    comparisons->layout()->addWidget(ancestor);
    layout->addWidget(comparisons);

    QGroupBox *commit = group(tr("Commit Message"), page);
    auto *showMargin = boolSetting(tr("Show right margin"), QStringLiteral("vc-show-commit-margin"), commit);
    commit->layout()->addWidget(showMargin);
    auto *marginForm = new QFormLayout;
    auto *margin = new QSpinBox(commit);
    margin->setRange(70, 120);
    margin->setValue(settings->intValue(QStringLiteral("vc-commit-margin")));
    connect(margin, &QSpinBox::valueChanged, this,
            [](int value) { MeldSettings::instance()->setValue(QStringLiteral("vc-commit-margin"), value); });
    marginForm->addRow(tr("Margin position"), margin);
    addGroupLayout(commit, marginForm);
    auto *breakLines = boolSetting(tr("Automatically &break lines at right margin on commit"),
                                   QStringLiteral("vc-break-commit-message"), commit);
    commit->layout()->addWidget(breakLines);
    // The margin settings only apply when the margin is shown
    const auto updateSensitivity = [showMargin, margin, breakLines]() {
        margin->setEnabled(showMargin->isChecked());
        breakLines->setEnabled(showMargin->isChecked());
    };
    connect(showMargin, &QCheckBox::toggled, this, updateSensitivity);
    updateSensitivity();
    layout->addWidget(commit);
    layout->addStretch(1);
    return page;
}

QWidget *PreferencesDialog::buildFiltersPage()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);

    QGroupBox *filenames = group(tr("Filename filters"), page);
    auto *filenameHelp = new QLabel(tr("When performing directory comparisons, you may filter out files and "
                                       "directories by name. Each pattern is a list of shell style wildcards "
                                       "separated by spaces."),
                                    filenames);
    filenameHelp->setWordWrap(true);
    filenames->layout()->addWidget(filenameHelp);
    filenames->layout()->addWidget(
        new FilterListWidget(QStringLiteral("filename-filters"), FilterEntry::Shell, filenames));
    layout->addWidget(filenames);

    QGroupBox *trimming = group(tr("Change trimming"), page);
    trimming->layout()->addWidget(boolSetting(tr("Trim blank line differences from the start and end of changes"),
                                              QStringLiteral("ignore-blank-lines"), trimming));
    layout->addWidget(trimming);

    QGroupBox *text = group(tr("Text filters"), page);
    auto *textHelp = new QLabel(tr("When performing file comparisons, you may ignore certain types of changes. Each "
                                   "pattern here is a python regular expression which replaces matching text with "
                                   "the empty string before comparison is performed. If the expression contains "
                                   "groups, only the groups are replaced."),
                                text);
    textHelp->setWordWrap(true);
    text->layout()->addWidget(textHelp);
    text->layout()->addWidget(new FilterListWidget(QStringLiteral("text-filters"), FilterEntry::Regex, text));
    layout->addWidget(text);
    layout->addStretch(1);
    return page;
}

} // namespace meld
