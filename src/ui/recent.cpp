#include "recent.h"

#include "core/iohelpers.h"
#include "core/misc.h"
#include "settings.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QUrl>

#include <stdexcept>

namespace meld {

QList<Comparison> RecentComparisons::load()
{
    QList<Comparison> result;
    const QString stored = MeldSettings::instance()->stringValue(QStringLiteral("recent-comparisons"));
    for (const QJsonValue &value : QJsonDocument::fromJson(stored.toUtf8()).array()) {
        const QJsonObject object = value.toObject();
        const std::optional<RecentType> type = recentTypeFromName(object.value(QStringLiteral("type")).toString());
        if (!type)
            continue;
        Comparison comparison;
        comparison.type = *type;
        for (const QJsonValue &path : object.value(QStringLiteral("paths")).toArray())
            comparison.paths.append(path.toString());
        result.append(comparison);
    }
    return result;
}

void RecentComparisons::add(const Comparison &comparison)
{
    // While Meld handles comparisons including blank panes, recording these
    // as recently-used comparisons just isn't that sane.
    if (comparison.paths.isEmpty() || comparison.paths.contains(QString()))
        return;
    for (const QString &path : comparison.paths) {
        if (path.startsWith(QDir::tempPath()))
            return;
    }

    QList<Comparison> entries = load();
    entries.removeIf([&comparison](const Comparison &c) {
        return c.type == comparison.type && c.paths == comparison.paths;
    });
    entries.prepend(comparison);
    while (entries.size() > MaxEntries)
        entries.removeLast();

    QJsonArray array;
    for (const Comparison &c : std::as_const(entries)) {
        array.append(QJsonObject{
            {QStringLiteral("type"), recentTypeName(c.type)},
            {QStringLiteral("paths"), QJsonArray::fromStringList(c.paths)},
        });
    }
    MeldSettings::instance()->setValue(QStringLiteral("recent-comparisons"),
                                       QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact)));
}

void RecentComparisons::clear()
{
    MeldSettings::instance()->setValue(QStringLiteral("recent-comparisons"), QStringLiteral("[]"));
}

QString RecentComparisons::displayName(const Comparison &comparison)
{
    if (comparison.paths.size() > 1)
        return shortenNames(comparison.paths).join(QStringLiteral(" : "));
    return QObject::tr("Version control:") + QLatin1Char(' ') + formatHomeRelativePath(comparison.paths.value(0));
}

Comparison RecentComparisons::readComparisonFile(const QString &path)
{
    QFile file(path);
    if (!file.exists())
        throw std::runtime_error("Recent comparison file does not exist");
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        throw std::runtime_error("Couldn't read recent comparison file");

    // A minimal reader for the configparser format: we only need the
    // [Comparison] section's "type" and "uris" options.
    QString section;
    QHash<QString, QString> options;
    const QStringList lines = QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'));
    for (const QString &rawLine : lines) {
        const QString line = rawLine.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')) || line.startsWith(QLatin1Char(';')))
            continue;
        if (line.startsWith(QLatin1Char('[')) && line.endsWith(QLatin1Char(']'))) {
            section = line.mid(1, line.size() - 2);
            continue;
        }
        if (section != QLatin1String("Comparison"))
            continue;
        const qsizetype separator = line.indexOf(QRegularExpression(QStringLiteral("[=:]")));
        if (separator < 0)
            throw std::runtime_error("Invalid recent comparison file");
        options.insert(line.left(separator).trimmed().toLower(), line.mid(separator + 1).trimmed());
    }

    const std::optional<RecentType> type = recentTypeFromName(options.value(QStringLiteral("type")));
    if (!type || !options.contains(QStringLiteral("uris")))
        throw std::runtime_error("Invalid recent comparison file");

    Comparison comparison;
    comparison.type = *type;
    for (const QString &uri : options.value(QStringLiteral("uris")).split(QLatin1Char(';'))) {
        const QUrl url = QUrl::fromEncoded(uri.toUtf8());
        comparison.paths.append(url.isLocalFile() ? url.toLocalFile() : uri);
    }
    return comparison;
}

} // namespace meld
