// Qt port of GtkSourceView's style schemes (gtksourcestylescheme.c,
// gtksourcestyleschememanager.c and gtksourcestyle.c)

#include "core/syntax/stylescheme.h"

#include <QCollator>
#include <QDir>
#include <QFile>
#include <QLoggingCategory>
#include <QLocale>
#include <QRegularExpression>
#include <QXmlStreamReader>

#include <algorithm>
#include <cmath>

namespace meld::syntax {

Q_LOGGING_CATEGORY(lcStyleScheme, "meld.syntax.style")

namespace {

bool parseBool(QStringView value)
{
    return value.compare(u"true", Qt::CaseInsensitive) == 0 || value.compare(u"yes", Qt::CaseInsensitive) == 0
        || value.compare(u"1", Qt::CaseInsensitive) == 0;
}

//! The value of an ASCII hex digit, or -1
int hexDigit(QChar c)
{
    const char16_t u = c.unicode();
    if (u >= u'0' && u <= u'9')
        return u - u'0';
    if (u >= u'a' && u <= u'f')
        return u - u'a' + 10;
    if (u >= u'A' && u <= u'F')
        return u - u'A' + 10;
    return -1;
}

//! Hex digits, as Pango reads them: only digits, no signs or prefixes
std::optional<unsigned> hexValue(QStringView digits)
{
    unsigned value = 0;
    for (const QChar c : digits) {
        const int digit = hexDigit(c);
        if (digit < 0)
            return std::nullopt;
        value = value * 16 + unsigned(digit);
    }
    return value;
}

//! Scale a hex component of `digits` digits to 16 bits, as Pango does
quint16 scaleHex(unsigned value, qsizetype digits)
{
    int bits = int(digits) * 4;
    value <<= 16 - bits;
    while (bits < 16) {
        value |= value >> bits;
        bits *= 2;
    }
    return quint16(value);
}

//! "#rgb" to "#rrrrggggbbbb", and with alpha "#rgba" to "#rrrrggggbbbbaaaa"
//! (pango_color_parse_with_alpha())
std::optional<QColor> parseHex(QStringView hex)
{
    const qsizetype length = hex.size();
    qsizetype digits = 0;
    bool alpha = false;
    switch (length) {
    case 3:
    case 6:
    case 9:
    case 12:
        digits = length / 3;
        break;
    case 4:
    case 8:
    case 16:
        digits = length / 4;
        alpha = true;
        break;
    default:
        return std::nullopt;
    }
    quint16 components[4] = {0, 0, 0, 0xffff};
    for (int i = 0; i < (alpha ? 4 : 3); ++i) {
        const std::optional<unsigned> value = hexValue(hex.mid(i * digits, digits));
        if (!value)
            return std::nullopt;
        components[i] = scaleHex(*value, digits);
    }
    return QColor::fromRgba64(components[0], components[1], components[2], components[3]);
}

bool isAsciiSpace(QChar c)
{
    return c == u' ' || c == u'\t' || c == u'\n' || c == u'\v' || c == u'\f' || c == u'\r';
}

//! g_ascii_strtod(): the number at the start of `text`, after any
//! whitespace; `used` is set to the number of characters read
std::optional<double> parseNumber(QStringView text, qsizetype *used)
{
    qsizetype i = 0;
    while (i < text.size() && isAsciiSpace(text.at(i)))
        ++i;
    bool negative = false;
    if (i < text.size() && (text.at(i) == u'+' || text.at(i) == u'-'))
        negative = text.at(i++) == u'-';
    const auto isDigit = [&text](qsizetype at) {
        return at < text.size() && text.at(at).unicode() >= u'0' && text.at(at).unicode() <= u'9';
    };
    const auto isHexDigit = [&text](qsizetype at) { return at < text.size() && hexDigit(text.at(at)) >= 0; };

    double value = 0;
    if (i + 1 < text.size() && text.at(i) == u'0' && (text.at(i + 1) == u'x' || text.at(i + 1) == u'X')
        && isHexDigit(i + 2)) {
        // Hexadecimal integers
        for (i += 2; isHexDigit(i); ++i)
            value = value * 16 + hexDigit(text.at(i));
    } else {
        const qsizetype start = i;
        qsizetype digits = 0;
        for (; isDigit(i); ++i)
            ++digits;
        if (i < text.size() && text.at(i) == u'.') {
            for (++i; isDigit(i); ++i)
                ++digits;
        }
        if (digits == 0)
            return std::nullopt;
        if (i < text.size() && (text.at(i) == u'e' || text.at(i) == u'E')) {
            qsizetype exponent = i + 1;
            if (exponent < text.size() && (text.at(exponent) == u'+' || text.at(exponent) == u'-'))
                ++exponent;
            if (isDigit(exponent)) {
                for (i = exponent; isDigit(i); ++i) {
                }
            }
        }
        bool ok = false;
        value = QLocale::c().toDouble(text.sliced(start, i - start), &ok);
        // Out of range
        if (!ok)
            return std::nullopt;
    }
    if (!std::isfinite(value))
        return std::nullopt;
    *used = i;
    return negative ? -value : value;
}

//! "rgb(r,g,b)" and "rgba(r,g,b,a)", as gdk_rgba_parse() reads them: the
//! components are numbers from 0 to 255 or percentages, and the alpha is
//! a number from 0 to 1
std::optional<QColor> parseFunction(QStringView text)
{
    const bool hasAlpha = text.startsWith(u"rgba");
    if (!hasAlpha && !text.startsWith(u"rgb"))
        return std::nullopt;
    qsizetype i = hasAlpha ? 4 : 3;
    const auto skipSpaces = [&]() {
        while (i < text.size() && text.at(i) == u' ')
            ++i;
    };
    const auto expect = [&](char16_t c) {
        skipSpaces();
        if (i >= text.size() || text.at(i) != c)
            return false;
        ++i;
        return true;
    };

    if (!expect(u'('))
        return std::nullopt;
    double values[4] = {0, 0, 0, 1};
    for (int component = 0; component < (hasAlpha ? 4 : 3); ++component) {
        if (component > 0 && !expect(u','))
            return std::nullopt;
        skipSpaces();
        qsizetype used = 0;
        const std::optional<double> value = parseNumber(text.sliced(i), &used);
        if (!value)
            return std::nullopt;
        i += used;
        if (component == 3) {
            values[component] = std::clamp(*value, 0.0, 1.0);
            continue;
        }
        skipSpaces();
        if (i < text.size() && text.at(i) == u'%') {
            ++i;
            values[component] = std::clamp(*value / 100.0, 0.0, 1.0);
        } else {
            values[component] = std::clamp(*value / 255.0, 0.0, 1.0);
        }
    }
    if (!expect(u')'))
        return std::nullopt;
    skipSpaces();
    if (i != text.size())
        return std::nullopt;
    return QColor::fromRgbF(float(values[0]), float(values[1]), float(values[2]), float(values[3]));
}

} // namespace

std::optional<QColor> parseColor(QStringView text)
{
    if (text.startsWith(u'#'))
        return parseHex(text.sliced(1));
    if (std::optional<QColor> color = parseFunction(text))
        return color;
    if (text.startsWith(u"rgb"))
        return std::nullopt;
    // Colour names, ignoring case and spaces as Pango does. Qt knows the
    // CSS names, which Pango has with the same values, but not Pango's
    // further X11 names (e.g., "grey50"), which schemes don't use.
    QString name = text.toString();
    name.remove(u' ');
    if (name.isEmpty() || name.startsWith(u'#') || name.compare(u"transparent", Qt::CaseInsensitive) == 0)
        return std::nullopt;
    const QColor color = QColor::fromString(name);
    if (color.isValid())
        return color;
    return std::nullopt;
}

std::optional<double> parseScale(QStringView text)
{
    // Pango's named scale factors (PANGO_SCALE_*)
    static const std::pair<const char16_t *, double> names[] = {
        {u"xx-small", 1.0 / (1.2 * 1.2 * 1.2)},
        {u"x-small", 1.0 / (1.2 * 1.2)},
        {u"small", 1.0 / 1.2},
        {u"medium", 1.0},
        {u"large", 1.2},
        {u"x-large", 1.2 * 1.2},
        {u"xx-large", 1.2 * 1.2 * 1.2},
    };
    for (const auto &[name, factor] : names) {
        if (text.compare(QStringView(name), Qt::CaseInsensitive) == 0)
            return factor;
    }
    // A number, ignoring anything after it as g_ascii_strtod() does
    static const QRegularExpression number(QStringLiteral(R"(^\s*[+-]?(\d+\.?\d*|\.\d+)([eE][+-]?\d+)?)"));
    const QRegularExpressionMatch match = number.matchView(text);
    if (!match.hasMatch())
        return std::nullopt;
    bool ok = false;
    const double value = QLocale::c().toDouble(match.capturedView().trimmed(), &ok);
    if (!ok || !(value > 0))
        return std::nullopt;
    return value;
}

// ---------------------------------------------------------------------------
// StyleScheme

std::unique_ptr<StyleScheme> StyleScheme::fromFile(const QString &fileName, QString *error)
{
    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = file.errorString();
        return nullptr;
    }
    return fromData(file.readAll(), fileName, error);
}

std::unique_ptr<StyleScheme> StyleScheme::fromData(const QByteArray &data, const QString &fileName, QString *error)
{
    std::unique_ptr<StyleScheme> scheme(new StyleScheme);
    scheme->fileName_ = fileName;
    const auto fail = [error](const QString &message) -> std::unique_ptr<StyleScheme> {
        if (error)
            *error = message;
        return nullptr;
    };

    QXmlStreamReader xml(data);
    if (!xml.readNextStartElement())
        return fail(QStringLiteral("empty document"));
    if (xml.name() != QLatin1String("style-scheme"))
        return fail(QStringLiteral("unexpected element '%1'").arg(xml.name()));

    const QXmlStreamAttributes attributes = xml.attributes();
    if (!attributes.hasAttribute(QLatin1String("version")))
        return fail(QStringLiteral("missing 'version' attribute"));
    if (attributes.value(QLatin1String("version")) != QLatin1String("1.0"))
        return fail(QStringLiteral("unsupported version '%1'").arg(attributes.value(QLatin1String("version"))));
    if (!attributes.hasAttribute(QLatin1String("id")))
        return fail(QStringLiteral("missing 'id' attribute"));
    scheme->id_ = attributes.value(QLatin1String("id")).toString();
    if (attributes.hasAttribute(QLatin1String("_name")))
        scheme->name_ = attributes.value(QLatin1String("_name")).toString();
    else if (attributes.hasAttribute(QLatin1String("name")))
        scheme->name_ = attributes.value(QLatin1String("name")).toString();
    else
        return fail(QStringLiteral("missing 'name' attribute"));
    scheme->parentId_ = attributes.value(QLatin1String("parent-scheme")).toString();

    while (xml.readNextStartElement()) {
        const QStringView name = xml.name();
        const QXmlStreamAttributes a = xml.attributes();
        if (name == QLatin1String("style")) {
            if (!a.hasAttribute(QLatin1String("name")))
                return fail(QStringLiteral("name attribute missing"));
            const QString styleName = a.value(QLatin1String("name")).toString();
            RawStyle style;
            if (a.hasAttribute(QLatin1String("use-style"))) {
                const QString used = a.value(QLatin1String("use-style")).toString();
                // Styles are only known after they're defined
                if (!scheme->styles_.contains(used))
                    return fail(QStringLiteral("in style '%1': unknown style '%2'").arg(styleName, used));
                for (const char *attribute : {"foreground", "background", "line-background", "italic", "bold",
                                              "strikethrough", "underline", "underline-color", "scale"}) {
                    if (a.hasAttribute(QLatin1String(attribute)))
                        return fail(QStringLiteral("in style '%1': style attributes used along with use-style")
                                        .arg(styleName));
                }
                style = scheme->styles_.value(used);
            } else {
                style.foreground = a.value(QLatin1String("foreground")).toString();
                style.background = a.value(QLatin1String("background")).toString();
                style.lineBackground = a.value(QLatin1String("line-background")).toString();
                style.underlineColor = a.value(QLatin1String("underline-color")).toString();
                const auto boolean = [&a](const char *attribute) -> std::optional<bool> {
                    if (!a.hasAttribute(QLatin1String(attribute)))
                        return std::nullopt;
                    return parseBool(a.value(QLatin1String(attribute)));
                };
                style.bold = boolean("bold");
                style.italic = boolean("italic");
                style.strikethrough = boolean("strikethrough");
                if (a.hasAttribute(QLatin1String("underline"))) {
                    const QStringView underline = a.value(QLatin1String("underline"));
                    // Up until 3.16 underline was a boolean, so "true" means
                    // "single"; other values are Pango's underline names.
                    static const QStringList nicks{QStringLiteral("none"),        QStringLiteral("single"),
                                                   QStringLiteral("double"),      QStringLiteral("low"),
                                                   QStringLiteral("error"),       QStringLiteral("single-line"),
                                                   QStringLiteral("double-line"), QStringLiteral("error-line")};
                    if (parseBool(underline))
                        style.underline = 1;
                    else if (const qsizetype index = nicks.indexOf(underline.toString().toLower()); index >= 0)
                        style.underline = int(index);
                }
                if (a.hasAttribute(QLatin1String("scale")))
                    style.scale = parseScale(a.value(QLatin1String("scale")));
            }
            scheme->styles_.insert(styleName, style);
            xml.skipCurrentElement();
        } else if (name == QLatin1String("color")) {
            const QString colorName = a.value(QLatin1String("name")).toString();
            const QString value = a.value(QLatin1String("value")).toString();
            if (colorName.isEmpty())
                return fail(QStringLiteral("name attribute missing in 'color' tag"));
            if (!a.hasAttribute(QLatin1String("value")))
                return fail(QStringLiteral("value attribute missing in 'color' tag"));
            if (!value.startsWith(QLatin1Char('#')) || value.size() < 2)
                return fail(QStringLiteral("value in 'color' tag is not of the form '#RGB' or '#name'"));
            if (scheme->namedColors_.contains(colorName))
                return fail(QStringLiteral("duplicated color '%1'").arg(colorName));
            scheme->namedColors_.insert(colorName, value);
            xml.skipCurrentElement();
        } else if (name == QLatin1String("author")) {
            scheme->authors_.append(xml.readElementText(QXmlStreamReader::IncludeChildElements));
        } else if (name == QLatin1String("description") || name == QLatin1String("_description")) {
            scheme->description_ = xml.readElementText(QXmlStreamReader::IncludeChildElements);
        } else if (name == QLatin1String("metadata")) {
            // Used by newer schemes, e.g., for light and dark variants
            while (xml.readNextStartElement()) {
                if (xml.name() == QLatin1String("property")) {
                    const QString propertyName = xml.attributes().value(QLatin1String("name")).toString();
                    scheme->metadata_.insert(propertyName, xml.readElementText().trimmed());
                } else {
                    xml.skipCurrentElement();
                }
            }
        } else {
            return fail(QStringLiteral("unknown node '%1'").arg(name));
        }
    }
    if (xml.hasError())
        return fail(xml.errorString());
    return scheme;
}

void StyleScheme::setParent(const StyleScheme *parent)
{
    parent_ = parent;
    cache_.clear();
}

QString StyleScheme::colorByName(const QString &name) const
{
    if (name.startsWith(QLatin1Char('#'))) {
        if (parseColor(QStringView(name).sliced(1)))
            return name.mid(1);
        if (parseColor(name))
            return name;
        qCWarning(lcStyleScheme) << "could not parse color" << name;
        return {};
    }
    if (const auto it = namedColors_.constFind(name); it != namedColors_.constEnd())
        return *it;
    if (parent_)
        return parent_->colorByName(name);
    qCWarning(lcStyleScheme) << "no color named" << name;
    return {};
}

std::optional<QColor> StyleScheme::resolveColor(const QString &name) const
{
    if (name.isEmpty())
        return std::nullopt;
    const QString color = colorByName(name);
    if (color.isEmpty())
        return std::nullopt;
    return parseColor(color);
}

std::optional<Style> StyleScheme::styleInternal(const QString &styleId) const
{
    if (const auto it = cache_.constFind(styleId); it != cache_.constEnd())
        return *it;

    std::optional<Style> result;
    if (const auto it = styles_.constFind(styleId); it != styles_.constEnd()) {
        Style style;
        style.foreground = resolveColor(it->foreground);
        style.background = resolveColor(it->background);
        style.lineBackground = resolveColor(it->lineBackground);
        style.underlineColor = resolveColor(it->underlineColor);
        style.bold = it->bold;
        style.italic = it->italic;
        style.strikethrough = it->strikethrough;
        style.underline = it->underline;
        style.scale = it->scale;
        result = style;
    } else if (parent_) {
        result = parent_->style(styleId);
    }
    cache_.insert(styleId, result);
    return result;
}

std::optional<Style> StyleScheme::style(const QString &styleId) const
{
    std::optional<Style> result = styleInternal(styleId);
    // "underlined" used to be the style for URLs; schemes may still only
    // have that, so it's the fallback for "net-address".
    if (!result && styleId == QLatin1String("def:net-address"))
        result = styleInternal(QStringLiteral("def:underlined"));
    return result;
}

// ---------------------------------------------------------------------------
// StyleSchemeManager

StyleSchemeManager &StyleSchemeManager::instance()
{
    static StyleSchemeManager manager({QStringLiteral(":/styles")});
    return manager;
}

StyleSchemeManager::StyleSchemeManager(const QStringList &searchPath)
{
    for (const QString &folder : searchPath) {
        const QDir dir(folder);
        for (const QString &entry : dir.entryList({QStringLiteral("*.xml")}, QDir::Files, QDir::Name)) {
            QString error;
            std::unique_ptr<StyleScheme> scheme = StyleScheme::fromFile(dir.filePath(entry), &error);
            if (!scheme) {
                qCWarning(lcStyleScheme) << "Couldn't load style scheme" << dir.filePath(entry) << ":" << error;
                continue;
            }
            // A scheme with the same id in an earlier folder takes precedence
            if (byId_.contains(scheme->id()))
                continue;
            byId_.insert(scheme->id(), scheme.get());
            schemes_.push_back(std::move(scheme));
        }
    }

    // Schemes with missing parents, or cycles of parents, are dropped
    QSet<QString> invalid;
    for (const auto &scheme : schemes_) {
        QSet<QString> chain;
        const StyleScheme *current = scheme.get();
        while (current && !current->parentId().isEmpty()) {
            chain.insert(current->id());
            const QString parentId = current->parentId();
            if (chain.contains(parentId) || !byId_.contains(parentId)) {
                qCWarning(lcStyleScheme) << "Invalid parent scheme" << parentId << "for" << current->id();
                invalid.unite(chain);
                break;
            }
            current = byId_.value(parentId);
        }
    }
    for (const QString &id : std::as_const(invalid))
        byId_.remove(id);
    for (const auto &scheme : schemes_) {
        if (byId_.contains(scheme->id()) && !scheme->parentId().isEmpty())
            scheme->setParent(byId_.value(scheme->parentId()));
    }

    // Sort by name
    std::vector<const StyleScheme *> sorted;
    for (const auto &scheme : schemes_) {
        if (byId_.value(scheme->id()) == scheme.get())
            sorted.push_back(scheme.get());
    }
    QCollator collator;
    std::stable_sort(sorted.begin(), sorted.end(), [&collator](const StyleScheme *a, const StyleScheme *b) {
        return collator.compare(a->name(), b->name()) < 0;
    });
    for (const StyleScheme *scheme : sorted)
        ids_.append(scheme->id());
}

StyleSchemeManager::~StyleSchemeManager() = default;

const StyleScheme *StyleSchemeManager::scheme(const QString &id) const
{
    return byId_.value(id);
}

} // namespace meld::syntax
