#include "style.h"

#include "icons.h"
#include "settings.h"

#include "core/syntax/stylescheme.h"

#include <QApplication>
#include <QSettings>
#include <QStandardPaths>
#include <QStyleHints>

namespace meld {

namespace {

//! Font zoom offset in points, applied to the configured font size
int fontSizeOffset = 0;

Theme makeLightTheme()
{
    // meld-base.style-scheme.xml
    Theme t;
    t.dark = false;
    t.insertFill = QColor(0xd0, 0xff, 0xa3);
    t.insertForeground = QColor(0x00, 0x88, 0x00);
    t.insertLine = QColor(0xa5, 0xff, 0x4c);
    t.replaceFill = QColor(0xbd, 0xdd, 0xff);
    t.replaceForeground = QColor(0x00, 0x44, 0xdd);
    t.replaceLine = QColor(0x65, 0xb2, 0xff);
    t.conflictFill = QColor(0xff, 0xa5, 0xa3);
    t.conflictForeground = QColor(0xff, 0x00, 0x00);
    t.conflictLine = QColor(0xff, 0x4f, 0x4c);
    t.deleteForeground = QColor(0x88, 0x00, 0x00);
    t.errorFill = QColor(0xfc, 0xe9, 0x4f);
    t.errorForeground = QColor(0xfa, 0xad, 0x3d);
    t.errorLine = QColor(0xfa, 0xde, 0x0a);
    t.inlineBackground = QColor(0x8a, 0xc2, 0xff);
    t.focusHighlight = QColor(0x33, 0x33, 0x33);
    t.currentLineHighlight = QColor(255, 255, 0, int(0.25 * 255));
    t.unknownForeground = QColor(0x88, 0x88, 0x88);
    t.syncpointOutline = QColor(0x55, 0x55, 0x55);
    t.currentChunkHighlight = QColor(255, 255, 255, int(0.5 * 255));
    t.overscroll = QColor(50, 50, 50, int(0.1 * 255));
    t.dimmedForeground = QColor(0x99, 0x99, 0x99);
    t.mapOverlay = QColor(100, 100, 100, int(0.4 * 255));
    t.searchMatch = QColor(0xfc, 0xe9, 0x4f, 200);
    return t;
}

Theme makeDarkTheme()
{
    // meld-dark.style-scheme.xml
    Theme t;
    t.dark = true;
    t.insertFill = QColor(0x12, 0x38, 0x06);
    t.insertForeground = QColor(0x4e, 0x62, 0x06);
    t.insertLine = QColor(0x24, 0x55, 0x15);
    t.replaceFill = QColor(0x00, 0x32, 0x66);
    t.replaceForeground = QColor(0x1d, 0x59, 0xd6);
    t.replaceLine = QColor(0x00, 0x53, 0xa6);
    t.conflictFill = QColor(0x7a, 0x2a, 0x28);
    t.conflictForeground = QColor(0xff, 0x00, 0x00);
    t.conflictLine = QColor(0xac, 0x3b, 0x39);
    t.deleteForeground = QColor(0xa4, 0x00, 0x00);
    t.errorFill = QColor(0xfc, 0xe9, 0x4f);
    t.errorForeground = QColor(0xfa, 0xad, 0x3d);
    t.errorLine = QColor(0xfd, 0xf8, 0xcd);
    t.inlineBackground = QColor(0x24, 0x52, 0x7e);
    t.focusHighlight = QColor(0xee, 0xee, 0xee);
    t.currentLineHighlight = QColor(255, 255, 0, int(0.15 * 255));
    t.unknownForeground = QColor(0xaa, 0xaa, 0xaa);
    t.syncpointOutline = QColor(0xbb, 0xbb, 0xbb);
    t.currentChunkHighlight = QColor(255, 255, 255, int(0.06 * 255));
    t.overscroll = QColor(255, 255, 255, int(0.1 * 255));
    t.dimmedForeground = QColor(0x99, 0x99, 0x99);
    t.mapOverlay = QColor(200, 200, 200, int(0.4 * 255));
    t.searchMatch = QColor(0x8a, 0x6d, 0x00, 200);
    return t;
}

Theme &themeStorage()
{
    static Theme theme = makeLightTheme();
    return theme;
}

bool themeInitialised = false;
const syntax::StyleScheme *styleScheme = nullptr;
const syntax::StyleScheme *baseScheme = nullptr;

//! Whether the application uses a dark style (Adw.StyleManager:dark); the
//! palette always matches the style (see applyStyleVariant())
bool applicationIsDark()
{
    return shouldUseDark(QApplication::palette().color(QPalette::Window));
}

constexpr char KdeColorSchemeProperty[] = "KDE_COLOR_SCHEME_PATH";

//! A colour of a KDE colour scheme file, e.g., "Window/BackgroundNormal"
std::optional<QColor> kdeSchemeColour(QSettings &scheme, const QString &key)
{
    const QStringList rgb = scheme.value(QStringLiteral("Colors:") + key).toStringList();
    if (rgb.size() != 3)
        return std::nullopt;
    return QColor(rgb.at(0).toInt(), rgb.at(1).toInt(), rgb.at(2).toInt());
}

/*!
 * A palette for platforms that can't switch between light and dark styles,
 * with the colours of KDE's Breeze and Breeze Dark, or of a KDE colour
 * scheme file (as KColorScheme::createApplicationPalette())
 */
QPalette stylePalette(bool dark, const QString &kdeScheme = QString())
{
    struct Colours {
        QColor window, base, alternateBase, button, text, disabledText, placeholder, highlight, highlightedText,
            link, linkVisited, toolTip;
    };
    Colours c = dark ? Colours{QColor(0x20, 0x23, 0x26), QColor(0x14, 0x16, 0x18), QColor(0x1d, 0x1f, 0x22),
                                     QColor(0x29, 0x2c, 0x30), QColor(0xfc, 0xfc, 0xfc), QColor(0x6e, 0x71, 0x73),
                                     QColor(0xa1, 0xa9, 0xb1), QColor(0x3d, 0xae, 0xe9), QColor(0xfc, 0xfc, 0xfc),
                                     QColor(0x1d, 0x99, 0xf3), QColor(0x9b, 0x59, 0xb6), QColor(0x29, 0x2c, 0x30)}
                           : Colours{QColor(0xef, 0xf0, 0xf1), QColor(0xff, 0xff, 0xff), QColor(0xf7, 0xf7, 0xf7),
                                     QColor(0xfc, 0xfc, 0xfc), QColor(0x23, 0x26, 0x29), QColor(0xa0, 0xa1, 0xa3),
                                     QColor(0x70, 0x7d, 0x8a), QColor(0x3d, 0xae, 0xe9), QColor(0xff, 0xff, 0xff),
                                     QColor(0x29, 0x80, 0xb9), QColor(0x9b, 0x59, 0xb6), QColor(0xf7, 0xf7, 0xf7)};
    if (!kdeScheme.isEmpty()) {
        QSettings scheme(kdeScheme, QSettings::IniFormat);
        const std::pair<QColor *, const char *> keys[] = {
            {&c.window, "Window/BackgroundNormal"},
            {&c.base, "View/BackgroundNormal"},
            {&c.alternateBase, "View/BackgroundAlternate"},
            {&c.button, "Button/BackgroundNormal"},
            {&c.text, "View/ForegroundNormal"},
            {&c.disabledText, "View/ForegroundInactive"},
            {&c.highlight, "Selection/BackgroundNormal"},
            {&c.highlightedText, "Selection/ForegroundNormal"},
            {&c.link, "View/ForegroundLink"},
            {&c.linkVisited, "View/ForegroundVisited"},
            {&c.toolTip, "Tooltip/BackgroundNormal"},
        };
        for (const auto &[colour, key] : keys) {
            if (const std::optional<QColor> value = kdeSchemeColour(scheme, QLatin1String(key)))
                *colour = *value;
        }
    }
    QPalette palette;
    const auto set = [&palette](QPalette::ColorRole role, const QColor &colour, const QColor &disabled) {
        palette.setColor(QPalette::Active, role, colour);
        palette.setColor(QPalette::Inactive, role, colour);
        palette.setColor(QPalette::Disabled, role, disabled);
    };
    set(QPalette::Window, c.window, c.window);
    set(QPalette::WindowText, c.text, c.disabledText);
    set(QPalette::Base, c.base, c.window);
    set(QPalette::AlternateBase, c.alternateBase, c.window);
    set(QPalette::Button, c.button, c.window);
    set(QPalette::ButtonText, c.text, c.disabledText);
    set(QPalette::Text, c.text, c.disabledText);
    set(QPalette::BrightText, dark ? QColor(Qt::white) : QColor(Qt::black), c.disabledText);
    set(QPalette::PlaceholderText, c.placeholder, c.disabledText);
    set(QPalette::Highlight, c.highlight, c.disabledText);
    set(QPalette::HighlightedText, c.highlightedText, c.window);
    set(QPalette::Link, c.link, c.disabledText);
    set(QPalette::LinkVisited, c.linkVisited, c.disabledText);
    set(QPalette::ToolTipBase, c.toolTip, c.toolTip);
    set(QPalette::ToolTipText, c.text, c.disabledText);
    // The shades styles draw bevels and frames with
    set(QPalette::Light, c.button.lighter(dark ? 160 : 110), c.window.lighter(dark ? 160 : 110));
    set(QPalette::Midlight, c.button.lighter(dark ? 130 : 105), c.window.lighter(dark ? 130 : 105));
    set(QPalette::Mid, c.button.darker(dark ? 130 : 150), c.window.darker(dark ? 130 : 150));
    set(QPalette::Dark, c.button.darker(dark ? 160 : 200), c.window.darker(dark ? 160 : 200));
    set(QPalette::Shadow, QColor(0, 0, 0, dark ? 255 : 160), QColor(0, 0, 0, dark ? 255 : 160));
    set(QPalette::Accent, c.highlight, c.disabledText);
    return palette;
}

//! adapt_style_scheme(): the variant of a scheme for the application's
//! light or dark style, if it has one
const syntax::StyleScheme *adaptStyleScheme(const syntax::StyleScheme *scheme)
{
    if (!scheme)
        return nullptr;
    const QString desired = applicationIsDark() ? QStringLiteral("dark") : QStringLiteral("light");
    const QString variant = scheme->metadata(QStringLiteral("variant"));
    const QString otherVariant = scheme->metadata(desired + QLatin1String("-variant"));
    if (variant.isEmpty() || variant == desired || otherVariant.isEmpty())
        return scheme;
    const syntax::StyleScheme *variantScheme = syntax::StyleSchemeManager::instance().scheme(otherVariant);
    return variantScheme ? variantScheme : scheme;
}

std::optional<QColor> styleColour(const syntax::StyleScheme *scheme, const QString &name, StyleAttribute attribute)
{
    if (!scheme)
        return std::nullopt;
    const std::optional<syntax::Style> style = scheme->style(name);
    if (!style)
        return std::nullopt;
    switch (attribute) {
    case StyleAttribute::Foreground:
        return style->foreground;
    case StyleAttribute::Background:
        return style->background;
    case StyleAttribute::LineBackground:
        return style->lineBackground;
    }
    return std::nullopt;
}

Theme themeFromSchemes(bool dark)
{
    // The built-in colours are those of meld-base and meld-dark, in case
    // the schemes are somehow missing
    Theme t = dark ? makeDarkTheme() : makeLightTheme();
    t.dark = dark;
    const auto lookup = [](QColor &target, const char *name, StyleAttribute attribute) {
        if (const std::optional<QColor> colour = colourLookupWithFallback(QLatin1String(name), attribute))
            target = *colour;
    };
    using enum StyleAttribute;
    // get_common_theme()
    lookup(t.insertFill, "meld:insert", Background);
    lookup(t.conflictFill, "meld:conflict", Background);
    lookup(t.replaceFill, "meld:replace", Background);
    lookup(t.errorFill, "meld:error", Background);
    lookup(t.focusHighlight, "meld:current-line-highlight", Foreground);
    lookup(t.currentChunkHighlight, "meld:current-chunk-highlight", Background);
    lookup(t.overscroll, "meld:overscroll", Background);
    lookup(t.insertLine, "meld:insert", LineBackground);
    lookup(t.conflictLine, "meld:conflict", LineBackground);
    lookup(t.replaceLine, "meld:replace", LineBackground);
    lookup(t.errorLine, "meld:error", LineBackground);
    // Tree views
    lookup(t.unknownForeground, "meld:unknown-text", Foreground);
    lookup(t.insertForeground, "meld:insert", Foreground);
    lookup(t.replaceForeground, "meld:replace", Foreground);
    lookup(t.deleteForeground, "meld:delete", Foreground);
    lookup(t.errorForeground, "meld:error", Foreground);
    lookup(t.conflictForeground, "meld:conflict", Foreground);
    // Text views
    lookup(t.currentLineHighlight, "meld:current-line-highlight", Background);
    lookup(t.syncpointOutline, "meld:syncpoint-outline", Foreground);
    lookup(t.inlineBackground, "meld:inline", Background);
    lookup(t.dimmedForeground, "meld:dimmed", Foreground);
    lookup(t.mapOverlay, "map-overlay", Background);
    lookup(t.searchMatch, "search-match", Background);
    return t;
}

} // namespace

QColor Theme::fill(ChunkTag tag) const
{
    switch (tag) {
    case ChunkTag::Insert:
    case ChunkTag::Delete:
        return insertFill;
    case ChunkTag::Replace:
        return replaceFill;
    case ChunkTag::Conflict:
        return conflictFill;
    case ChunkTag::Equal:
        break;
    }
    return errorFill;
}

QColor Theme::line(ChunkTag tag) const
{
    switch (tag) {
    case ChunkTag::Insert:
    case ChunkTag::Delete:
        return insertLine;
    case ChunkTag::Replace:
        return replaceLine;
    case ChunkTag::Conflict:
        return conflictLine;
    case ChunkTag::Equal:
        break;
    }
    return errorLine;
}

QColor Theme::highlightedFill(ChunkTag tag) const
{
    return alphaTint(fill(tag), currentChunkHighlight.alphaF());
}

QColor alphaTint(const QColor &color, double alpha)
{
    return QColor::fromRgbF(float(alpha + color.redF() * (1.0 - alpha)), float(alpha + color.greenF() * (1.0 - alpha)),
                            float(alpha + color.blueF() * (1.0 - alpha)), float(alpha + color.alphaF() * (1.0 - alpha)));
}

bool shouldUseDark(const QColor &background)
{
    return (2.0 * background.redF() + 5.0 * background.greenF() + background.blueF()) < 4.0;
}

bool shouldUseDark(const syntax::StyleScheme *scheme)
{
    // If the style scheme has variant metadata, we will trust that
    if (scheme) {
        const QString variant = scheme->metadata(QStringLiteral("variant"));
        if (!variant.isEmpty())
            return variant == QLatin1String("dark");
    }
    // Otherwise check the text background of the scheme, falling back to
    // the application's background for schemes without one
    std::optional<QColor> background = styleColour(scheme, QStringLiteral("text"), StyleAttribute::Background);
    if (!background)
        background = qApp ? QApplication::palette().color(QPalette::Window) : QColor(Qt::white);
    return shouldUseDark(*background);
}

bool updateTheme()
{
    syntax::StyleSchemeManager &manager = syntax::StyleSchemeManager::instance();
    const syntax::StyleScheme *scheme =
        manager.scheme(MeldSettings::instance()->stringValue(QStringLiteral("style-scheme")));
    if (!scheme)
        scheme = manager.scheme(MeldSettings::defaultValue(QStringLiteral("style-scheme")).toString());
    scheme = adaptStyleScheme(scheme);

    // set_base_style_scheme()
    const bool dark = shouldUseDark(scheme);
    const syntax::StyleScheme *base = manager.scheme(dark ? QStringLiteral("meld-dark") : QStringLiteral("meld-base"));
    if (scheme && (scheme->id() == QLatin1String("meld-dark") || scheme->id() == QLatin1String("meld-base")))
        scheme = base;

    const bool changed = !themeInitialised || scheme != styleScheme || base != baseScheme || dark != themeStorage().dark;
    styleScheme = scheme;
    baseScheme = base;
    themeInitialised = true;
    themeStorage() = themeFromSchemes(dark);
    return changed;
}

const Theme &currentTheme()
{
    if (!themeInitialised && qApp)
        updateTheme();
    return themeStorage();
}

const syntax::StyleScheme *currentStyleScheme()
{
    if (!themeInitialised && qApp)
        updateTheme();
    return styleScheme;
}

const syntax::StyleScheme *baseStyleScheme()
{
    if (!themeInitialised && qApp)
        updateTheme();
    return baseScheme;
}

std::optional<QColor> schemeColour(const QString &name, StyleAttribute attribute)
{
    return styleColour(currentStyleScheme(), name, attribute);
}

QPalette schemeTextPalette(const syntax::StyleScheme *scheme)
{
    // The colours GtkSourceView applies to its views
    QPalette colours;
    const auto set = [&colours, scheme](QPalette::ColorGroup group, QPalette::ColorRole role, const char *style,
                                        StyleAttribute attribute) {
        if (const std::optional<QColor> colour = styleColour(scheme, QLatin1String(style), attribute)) {
            if (group == QPalette::All)
                colours.setColor(role, *colour);
            else
                colours.setColor(group, role, *colour);
        }
    };
    set(QPalette::All, QPalette::Text, "text", StyleAttribute::Foreground);
    set(QPalette::All, QPalette::Base, "text", StyleAttribute::Background);
    set(QPalette::All, QPalette::Highlight, "selection", StyleAttribute::Background);
    set(QPalette::All, QPalette::HighlightedText, "selection", StyleAttribute::Foreground);
    set(QPalette::Inactive, QPalette::Highlight, "selection-unfocused", StyleAttribute::Background);
    set(QPalette::Inactive, QPalette::HighlightedText, "selection-unfocused", StyleAttribute::Foreground);
    return colours;
}

std::optional<QColor> colourLookupWithFallback(const QString &name, StyleAttribute attribute)
{
    if (std::optional<QColor> colour = styleColour(currentStyleScheme(), name, attribute))
        return colour;
    return styleColour(baseStyleScheme(), name, attribute);
}

void applyStyleVariant()
{
    const QString variant = MeldSettings::instance()->stringValue(QStringLiteral("style-variant"));
    Qt::ColorScheme scheme = Qt::ColorScheme::Unknown;
    if (variant == QLatin1String("force-light"))
        scheme = Qt::ColorScheme::Light;
    else if (variant == QLatin1String("force-dark"))
        scheme = Qt::ColorScheme::Dark;
    // Platforms that support it switch their palette...
    QGuiApplication::styleHints()->setColorScheme(scheme);
    qApp->setProperty(KdeColorSchemeProperty, QVariant());
    QApplication::setPalette(QPalette());
    // ...but many don't (e.g., KDE's, or with Qt's Fusion style), so the
    // palette is made to match
    if (scheme != Qt::ColorScheme::Unknown) {
        const bool dark = scheme == Qt::ColorScheme::Dark;
        if (shouldUseDark(QApplication::palette().color(QPalette::Window)) != dark) {
            // KDE's Breeze style draws tool bars with the colours of the
            // application's KDE colour scheme, which KDE applications
            // choose with this property; it's read on palette changes.
            const QString kdeScheme = QStandardPaths::locate(
                QStandardPaths::GenericDataLocation,
                dark ? QStringLiteral("color-schemes/BreezeDark.colors") : QStringLiteral("color-schemes/BreezeLight.colors"));
            if (!kdeScheme.isEmpty())
                qApp->setProperty(KdeColorSchemeProperty, kdeScheme);
            QApplication::setPalette(stylePalette(dark, kdeScheme));
        }
    }
    icons::updateIconTheme();
    // Comparisons' colours depend on the style (as the original's
    // Adw.StyleManager:dark handler)
    if (themeInitialised && updateTheme())
        Q_EMIT MeldSettings::instance()->changed(QStringLiteral("style-scheme"));
}

void resetFontZoom()
{
    fontSizeOffset = 0;
}

void updateFontZoom(std::optional<int> change)
{
    MeldSettings *settings = MeldSettings::instance();

    // None/zero resets the zoom factor
    int offset = (change && *change) ? fontSizeOffset + *change : 0;

    // Ensure that we can't zoom out past 1pt
    const int fontSize = int(settings->font().pointSizeF());
    offset = std::max(offset, 1 - fontSize);
    if (offset == fontSizeOffset)
        return;

    fontSizeOffset = offset;
    // Several widgets hook into the font changed signal already; reusing it
    // here is the simplest option to keep them responsive
    Q_EMIT settings->changed(QStringLiteral("font"));
}

int fontZoomOffset()
{
    return fontSizeOffset;
}

QFont editorFont()
{
    QFont font = MeldSettings::instance()->font();
    const int fontSize = int(font.pointSizeF());
    font.setPointSize(std::max(1, fontSize + fontSizeOffset));
    return font;
}

} // namespace meld
