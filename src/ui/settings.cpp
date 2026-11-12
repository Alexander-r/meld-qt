#include "settings.h"

#include "style.h"

#include <QCoreApplication>
#include <QFontDatabase>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>

namespace meld {

namespace {

std::unique_ptr<MeldSettings> &instanceStorage()
{
    static std::unique_ptr<MeldSettings> settings;
    return settings;
}

struct DefaultFilter {
    const char *name;
    bool active;
    const char *pattern;
};

// The names are translated, as the original's GSettings schema has them
constexpr DefaultFilter FilenameFilters[] = {
    {QT_TRANSLATE_NOOP("meld::MeldSettings", "Backups"), true, R"(#*# .#* ~* *~ *.{orig,bak,swp})"},
    {QT_TRANSLATE_NOOP("meld::MeldSettings", "OS-Specific Metadata"), true,
     R"(.DS_Store ._* .Spotlight-V100 .Trashes Thumbs.db Desktop.ini)"},
    {QT_TRANSLATE_NOOP("meld::MeldSettings", "Version Control"), true,
     R"(_MTN .bzr .svn .svn .hg .fslckout _FOSSIL_ .fos CVS _darcs .git .svn .osc)"},
    {QT_TRANSLATE_NOOP("meld::MeldSettings", "Binaries"), true, R"(*.{pyc,a,obj,o,so,la,lib,dll,exe})"},
    {QT_TRANSLATE_NOOP("meld::MeldSettings", "Media"), false,
     R"(*.{jxl,jpg,jpeg,gif,png,avif,webp,heif,heic,bmp,tif,tiff,raw,dng,cr2,wav,wave,mp3,ogg,oga,vorbis,spx,opus,flac,ac3,aac,aif,aiff,aifc,alac,m4a,3gp,wma,aup,aup3,avi,mov,mpg,mpeg,mp4,m4v,webm,ogv,flv,xcf,xpm,ora,kra,psd,psp})"},
};

constexpr DefaultFilter TextFilters[] = {
    {QT_TRANSLATE_NOOP("meld::MeldSettings", "CVS/SVN Keywords"), false, R"(\$\w+(:[^\n$]+)?\$)"},
    {QT_TRANSLATE_NOOP("meld::MeldSettings", "C++ Comment"), false, R"(//.*)"},
    {QT_TRANSLATE_NOOP("meld::MeldSettings", "C Comment"), false, R"(/\*.*?\*/)"},
    {QT_TRANSLATE_NOOP("meld::MeldSettings", "All Whitespace"), false, R"([ \t\r\f\v]*)"},
    {QT_TRANSLATE_NOOP("meld::MeldSettings", "Leading Whitespace"), false, R"(^[ \t\r\f\v]*)"},
    {QT_TRANSLATE_NOOP("meld::MeldSettings", "Trailing Whitespace"), false, R"([ \t\r\f\v]*$)"},
    {QT_TRANSLATE_NOOP("meld::MeldSettings", "Script Comment"), false, R"(#.*)"},
};

//! The filters as they're stored in the settings, with translated names
template <size_t N>
QString defaultFilters(const DefaultFilter (&filters)[N])
{
    QJsonArray array;
    for (const DefaultFilter &filter : filters) {
        array.append(QJsonArray{QCoreApplication::translate("meld::MeldSettings", filter.name), filter.active,
                                QString::fromUtf8(filter.pattern)});
    }
    return QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact));
}

QString defaultFilenameFilters()
{
    return defaultFilters(FilenameFilters);
}

QString defaultTextFilters()
{
    return defaultFilters(TextFilters);
}

const QHash<QString, QVariant> &defaults()
{
    static const QHash<QString, QVariant> values{
        {QStringLiteral("style-variant"), QStringLiteral("default")},
        // Not in the original: a translation's language ("de", "pt_BR",
        // "en"), or empty for the system's
        {QStringLiteral("language"), QString()},
        {QStringLiteral("detect-encodings"), QStringList()},
        {QStringLiteral("indent-width"), 8},
        {QStringLiteral("insert-spaces-instead-of-tabs"), false},
        {QStringLiteral("show-line-numbers"), false},
        {QStringLiteral("highlight-syntax"), true},
        {QStringLiteral("style-scheme"), QStringLiteral("classic")},
        {QStringLiteral("enable-space-drawer"), false},
        {QStringLiteral("wrap-mode"), QStringLiteral("none")},
        {QStringLiteral("highlight-current-line"), false},
        {QStringLiteral("use-system-font"), true},
        {QStringLiteral("custom-font"), QStringLiteral("monospace, 14")},
        {QStringLiteral("show-overview-map"), true},
        {QStringLiteral("overview-map-style"), QStringLiteral("chunkmap")},
        {QStringLiteral("ignore-blank-lines"), false},
        {QStringLiteral("use-system-editor"), true},
        // Not in the original, which is always a single instance
        {QStringLiteral("single-instance"), false},
        {QStringLiteral("custom-editor-command"), QString()},
        {QStringLiteral("folder-columns"),
         QStringLiteral(R"([["size", true], ["modification time", true], ["permissions", false]])")},
        {QStringLiteral("folder-ignore-symlinks"), false},
        {QStringLiteral("folder-shallow-comparison"), false},
        {QStringLiteral("folder-time-resolution"), 100},
        {QStringLiteral("folder-filter-text"), true},
        {QStringLiteral("folder-status-filters"),
         QStringList{QStringLiteral("normal"), QStringLiteral("modified"), QStringLiteral("new")}},
        {QStringLiteral("vc-console-visible"), false},
        {QStringLiteral("vc-console-pane-position"), 300},
        {QStringLiteral("vc-left-is-local"), false},
        {QStringLiteral("vc-merge-file-order"), QStringLiteral("remote-merge-local")},
        {QStringLiteral("conflict-show-ancestor"), true},
        {QStringLiteral("vc-show-commit-margin"), true},
        {QStringLiteral("vc-commit-margin"), 72},
        {QStringLiteral("vc-break-commit-message"), false},
        {QStringLiteral("vc-status-filters"), QStringList{QStringLiteral("flatten"), QStringLiteral("modified")}},
        // Kept in a separate history.ini file by the original
        {QStringLiteral("commit-message-history"), QStringList()},
        {QStringLiteral("filename-filters"), defaultFilenameFilters()},
        {QStringLiteral("text-filters"), defaultTextFilters()},
        {QStringLiteral("window-width"), -1},
        {QStringLiteral("window-height"), -1},
        {QStringLiteral("window-is-maximized"), false},
        {QStringLiteral("recent-comparisons"), QStringLiteral("[]")},
    };
    return values;
}

std::unique_ptr<QSettings> userSettings()
{
    return std::make_unique<QSettings>(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("meld-qt"),
                                       QStringLiteral("meld-qt"));
}

} // namespace

MeldSettings::MeldSettings(std::unique_ptr<QSettings> settings)
    : settings_(std::move(settings))
{
    reloadFilters();
    reloadFont();
}

MeldSettings *MeldSettings::instance()
{
    auto &storage = instanceStorage();
    if (!storage)
        storage.reset(new MeldSettings(userSettings()));
    return storage.get();
}

QString MeldSettings::languageSetting()
{
    if (instanceStorage())
        return instance()->stringValue(QStringLiteral("language"));
    return userSettings()->value(QStringLiteral("language")).toString();
}

void MeldSettings::useSettingsFile(const QString &path)
{
    instanceStorage().reset(new MeldSettings(std::make_unique<QSettings>(path, QSettings::IniFormat)));
}

QVariant MeldSettings::defaultValue(const QString &key)
{
    return defaults().value(key);
}

QVariant MeldSettings::value(const QString &key) const
{
    const QVariant fallback = defaultValue(key);
    QVariant stored = settings_->value(key, fallback);
    // QSettings' INI backend stores everything as strings, so convert back
    // to the schema type
    if (fallback.isValid() && stored.metaType() != fallback.metaType()) {
        if (fallback.metaType().id() == QMetaType::QStringList && stored.metaType().id() == QMetaType::QString)
            stored = stored.toString().isEmpty() ? QStringList() : QStringList{stored.toString()};
        else
            stored.convert(fallback.metaType());
    }
    return stored;
}

void MeldSettings::setValue(const QString &key, const QVariant &value)
{
    if (this->value(key) == value)
        return;
    settings_->setValue(key, value);
    if (key == QLatin1String("filename-filters") || key == QLatin1String("text-filters"))
        reloadFilters();
    if (key == QLatin1String("filename-filters"))
        Q_EMIT fileFiltersChanged();
    if (key == QLatin1String("text-filters"))
        Q_EMIT textFiltersChanged();
    if (key == QLatin1String("use-system-font") || key == QLatin1String("custom-font")) {
        reloadFont();
        resetFontZoom();
        Q_EMIT changed(QStringLiteral("font"));
    }
    // Listeners expect the scheme to be updated (as the original's
    // MeldSettings.style_scheme)
    if (key == QLatin1String("style-scheme"))
        updateTheme();
    // As MeldSettings.on_setting_changed() in the original
    if (key == QLatin1String("style-variant"))
        applyStyleVariant();
    Q_EMIT changed(key);
}

void MeldSettings::resetValue(const QString &key)
{
    setValue(key, defaultValue(key));
    settings_->remove(key);
}

QList<FilterSetting> MeldSettings::filterSettings(const QString &key) const
{
    QList<FilterSetting> result;
    const QJsonArray array = QJsonDocument::fromJson(stringValue(key).toUtf8()).array();
    for (const QJsonValue &entry : array) {
        const QJsonArray fields = entry.toArray();
        if (fields.size() != 3)
            continue;
        result.append({fields.at(0).toString(), fields.at(1).toBool(), fields.at(2).toString()});
    }
    return result;
}

void MeldSettings::setFilterSettings(const QString &key, const QList<FilterSetting> &filters)
{
    QJsonArray array;
    for (const FilterSetting &filter : filters)
        array.append(QJsonArray{filter.name, filter.active, filter.pattern});
    setValue(key, QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact)));
}

QList<ColumnSetting> MeldSettings::folderColumns() const
{
    QList<ColumnSetting> result;
    const QJsonArray array = QJsonDocument::fromJson(stringValue(QStringLiteral("folder-columns")).toUtf8()).array();
    for (const QJsonValue &entry : array) {
        const QJsonArray fields = entry.toArray();
        if (fields.size() == 2)
            result.append({fields.at(0).toString(), fields.at(1).toBool()});
    }
    return result;
}

void MeldSettings::setFolderColumns(const QList<ColumnSetting> &columns)
{
    QJsonArray array;
    for (const ColumnSetting &column : columns)
        array.append(QJsonArray{column.name, column.visible});
    setValue(QStringLiteral("folder-columns"), QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact)));
}

void MeldSettings::reloadFilters()
{
    fileFilters_.clear();
    for (const FilterSetting &f : filterSettings(QStringLiteral("filename-filters")))
        fileFilters_.append(FilterEntry::fromSettings(f.name, f.active, f.pattern, FilterEntry::Shell));
    textFilters_.clear();
    for (const FilterSetting &f : filterSettings(QStringLiteral("text-filters")))
        textFilters_.append(FilterEntry::fromSettings(f.name, f.active, f.pattern, FilterEntry::Regex));
}

QFont MeldSettings::parseFontDescription(const QString &description)
{
    // Parse a Pango-style description such as "Monospace Bold 12" or
    // "monospace, 14"
    static const QSet<QString> styleWords{
        QStringLiteral("bold"),    QStringLiteral("italic"), QStringLiteral("oblique"),
        QStringLiteral("light"),   QStringLiteral("medium"), QStringLiteral("semi-bold"),
        QStringLiteral("regular"), QStringLiteral("normal"), QStringLiteral("book"),
    };
    QStringList words = description.split(QRegularExpression(QStringLiteral("[\\s,]+")), Qt::SkipEmptyParts);
    QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    bool ok = false;
    if (!words.isEmpty()) {
        const double size = words.last().toDouble(&ok);
        if (ok && size > 0) {
            font.setPointSizeF(size);
            words.removeLast();
        }
    }
    bool bold = false;
    bool italic = false;
    while (!words.isEmpty() && styleWords.contains(words.last().toLower())) {
        const QString word = words.takeLast().toLower();
        bold = bold || word == QLatin1String("bold") || word == QLatin1String("semi-bold");
        italic = italic || word == QLatin1String("italic") || word == QLatin1String("oblique");
    }
    if (!words.isEmpty()) {
        const QString family = words.join(QLatin1Char(' '));
        if (family.compare(QLatin1String("monospace"), Qt::CaseInsensitive) == 0) {
            font.setStyleHint(QFont::Monospace);
            font.setFamily(QFontDatabase::systemFont(QFontDatabase::FixedFont).family());
        } else {
            font.setFamily(family);
        }
    }
    font.setBold(bold);
    font.setItalic(italic);
    return font;
}

QString MeldSettings::fontDescription(const QFont &font)
{
    QString description = font.family();
    if (font.bold())
        description += QStringLiteral(" Bold");
    if (font.italic())
        description += QStringLiteral(" Italic");
    description += QStringLiteral(" %1").arg(font.pointSizeF() > 0 ? font.pointSizeF() : 11.0);
    return description;
}

void MeldSettings::reloadFont()
{
    QString description;
    if (!boolValue(QStringLiteral("use-system-font")))
        description = stringValue(QStringLiteral("custom-font"));
    if (description.isEmpty()) {
        font_ = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        if (font_.pointSizeF() <= 0)
            font_.setPointSize(11);
    } else {
        font_ = parseFontDescription(description);
    }
}

} // namespace meld
