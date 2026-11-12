#pragma once

#include "core/diffchunk.h"

#include <QColor>
#include <QFont>
#include <QPalette>

#include <optional>

namespace meld {

namespace syntax {
class StyleScheme;
} // namespace syntax

//! The colours of comparisons: those of the current style scheme, or of
//! Meld's own schemes (meld-base and meld-dark) for those it doesn't have
struct Theme {
    bool dark = false;

    QColor insertFill;
    QColor replaceFill;
    QColor conflictFill;
    QColor errorFill;
    QColor insertLine;
    QColor replaceLine;
    QColor conflictLine;
    QColor errorLine;

    QColor insertForeground;
    QColor replaceForeground;
    QColor conflictForeground;
    QColor deleteForeground;
    QColor errorForeground;
    QColor unknownForeground;

    QColor inlineBackground;
    QColor currentLineHighlight;
    QColor focusHighlight;
    QColor syncpointOutline;
    QColor currentChunkHighlight;
    QColor overscroll;
    QColor dimmedForeground;
    QColor mapOverlay;
    QColor searchMatch;

    QColor fill(ChunkTag tag) const;
    QColor line(ChunkTag tag) const;
    //! Fill colour tinted with the current chunk highlight
    QColor highlightedFill(ChunkTag tag) const;
};

//! The theme of the current style scheme
const Theme &currentTheme();
/*!
 * Re-evaluate the style scheme and theme, after the "style-scheme" setting
 * or the palette changed (MeldSettings._style_scheme_from_gsettings())
 *
 * Returns whether the scheme or its light or dark style changed.
 */
bool updateTheme();

/*!
 * The style scheme for comparisons (MeldSettings.style_scheme): the
 * "style-scheme" setting, as the light or dark variant matching the
 * application's style. Meld's own schemes follow the application's style.
 */
const syntax::StyleScheme *currentStyleScheme();
//! Meld's scheme (meld-base or meld-dark) for the colours the current
//! scheme doesn't have
const syntax::StyleScheme *baseStyleScheme();

enum class StyleAttribute {
    Foreground,
    Background,
    LineBackground,
};

//! colour_lookup_with_fallback(): a colour of a style of the current
//! scheme, or of the base scheme if the current one doesn't have it
std::optional<QColor> colourLookupWithFallback(const QString &name, StyleAttribute attribute);
//! A colour of a style of the current scheme, if it has one
std::optional<QColor> schemeColour(const QString &name, StyleAttribute attribute);

//! A palette with a scheme's text and selection colours, for text views;
//! the other colours are left to the widget's palette
QPalette schemeTextPalette(const syntax::StyleScheme *scheme);

//! Dark mode detection as recommended by Microsoft
bool shouldUseDark(const QColor &background);
//! Whether a style scheme is dark, from its metadata or text background
bool shouldUseDark(const syntax::StyleScheme *scheme);

QColor alphaTint(const QColor &color, double alpha);

//! Apply the "style-variant" setting to the application colour scheme
void applyStyleVariant();

void resetFontZoom();
//! Change the font zoom by `change` points; nullopt or 0 resets the zoom
void updateFontZoom(std::optional<int> change);
int fontZoomOffset();
//! The comparison font with zoom applied
QFont editorFont();

} // namespace meld
