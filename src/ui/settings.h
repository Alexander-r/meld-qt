#pragma once

#include "core/filters.h"

#include <QFont>
#include <QObject>
#include <QSettings>
#include <QVariant>

#include <memory>

namespace meld {

struct FilterSetting {
    QString name;
    bool active = false;
    QString pattern;

    friend bool operator==(const FilterSetting &, const FilterSetting &) = default;
};

struct ColumnSetting {
    QString name;
    bool visible = false;

    friend bool operator==(const ColumnSetting &, const ColumnSetting &) = default;
};

/*!
 * Application settings, mirroring the org.gnome.meld GSettings schema.
 *
 * Simple keys are accessed by name with their schema defaults; filters,
 * columns and fonts have typed accessors. Like MeldSettings in Python, this
 * emits signals for changes that can't easily be bound to properties.
 */
class MeldSettings : public QObject {
    Q_OBJECT

public:
    static MeldSettings *instance();
    //! Use an isolated settings file (for tests)
    static void useSettingsFile(const QString &path);
    /*!
     * The language setting, read without making the settings if they aren't
     * made yet: their defaults are translated (the names of filters), so the
     * translation is installed first.
     */
    static QString languageSetting();

    QVariant value(const QString &key) const;
    void setValue(const QString &key, const QVariant &value);
    void resetValue(const QString &key);
    static QVariant defaultValue(const QString &key);

    bool boolValue(const QString &key) const { return value(key).toBool(); }
    int intValue(const QString &key) const { return value(key).toInt(); }
    QString stringValue(const QString &key) const { return value(key).toString(); }
    QStringList stringListValue(const QString &key) const { return value(key).toStringList(); }

    QList<FilterSetting> filterSettings(const QString &key) const;
    void setFilterSettings(const QString &key, const QList<FilterSetting> &filters);
    QList<ColumnSetting> folderColumns() const;
    void setFolderColumns(const QList<ColumnSetting> &columns);

    const FilterList &fileFilters() const { return fileFilters_; }
    const FilterList &textFilters() const { return textFilters_; }

    //! The configured comparison font, without zoom applied
    QFont font() const { return font_; }
    static QFont parseFontDescription(const QString &description);
    static QString fontDescription(const QFont &font);

    QSettings &backend() { return *settings_; }

Q_SIGNALS:
    //! A settings key changed; "font" is also emitted for zoom changes
    void changed(const QString &key);
    void fileFiltersChanged();
    void textFiltersChanged();

private:
    explicit MeldSettings(std::unique_ptr<QSettings> settings);
    void reloadFilters();
    void reloadFont();

    std::unique_ptr<QSettings> settings_;
    FilterList fileFilters_;
    FilterList textFilters_;
    QFont font_;
};

} // namespace meld
