// The GTK version uses symbolic icons from the icon theme. Here we look up
// the freedesktop names (preferring the platform's non-symbolic variants),
// and fall back to Qt's standard icons or simple drawn icons.

#pragma once

#include <QColor>
#include <QIcon>
#include <QString>

namespace meld::icons {

//! Ensure an icon theme is available, e.g. when no platform theme is set
void initialiseIconTheme();
//! Switch between light and dark variants of icon themes (e.g., Breeze and
//! Breeze Dark) to match the palette
void updateIconTheme();

QIcon icon(const QString &name);

//! A monochrome version of a named icon, like GTK's symbolic recolouring
QPixmap tintedPixmap(const QString &name, const QColor &color, int size, qreal devicePixelRatio);

} // namespace meld::icons
