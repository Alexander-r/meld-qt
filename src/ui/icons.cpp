#include "icons.h"

#include "style.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QIconEngine>
#include <QPainter>
#include <QPainterPath>
#include <QStandardPaths>
#include <QStyle>

#include <functional>

namespace meld::icons {

namespace {

struct IconSpec {
    QStringList themeNames;
    QStyle::StandardPixmap standard = QStyle::SP_CustomBase;
};

const QHash<QString, IconSpec> &iconSpecs()
{
    static const QHash<QString, IconSpec> specs{
        {"go-up-symbolic", {{"go-up", "go-up-symbolic", "arrow-up"}, QStyle::SP_ArrowUp}},
        {"go-down-symbolic", {{"go-down", "go-down-symbolic", "arrow-down"}, QStyle::SP_ArrowDown}},
        {"go-previous-symbolic", {{"go-previous", "go-previous-symbolic", "arrow-left"}, QStyle::SP_ArrowLeft}},
        {"go-next-symbolic", {{"go-next", "go-next-symbolic", "arrow-right"}, QStyle::SP_ArrowRight}},
        {"go-top-symbolic", {{"go-top", "go-top-symbolic", "go-first-view"}, QStyle::SP_ArrowUp}},
        {"go-bottom-symbolic", {{"go-bottom", "go-bottom-symbolic", "go-last-view"}, QStyle::SP_ArrowDown}},
        {"go-jump-symbolic", {{"go-jump", "go-jump-symbolic"}, QStyle::SP_ArrowForward}},
        {"tab-new-symbolic", {{"tab-new", "tab-new-symbolic", "document-new"}, QStyle::SP_FileIcon}},
        {"document-open-recent-symbolic",
         {{"document-open-recent", "document-open-recent-symbolic"}, QStyle::SP_DirOpenIcon}},
        {"open-menu-symbolic", {{"application-menu", "open-menu", "open-menu-symbolic"}}},
        {"edit-copy-symbolic", {{"edit-copy", "edit-copy-symbolic"}}},
        {"edit-delete-symbolic", {{"edit-delete", "edit-delete-symbolic"}, QStyle::SP_DialogDiscardButton}},
        {"user-trash-symbolic", {{"user-trash", "user-trash-symbolic"}, QStyle::SP_TrashIcon}},
        {"document-save-symbolic", {{"document-save", "document-save-symbolic"}, QStyle::SP_DialogSaveButton}},
        {"document-save-as-symbolic",
         {{"document-save-as", "document-save-as-symbolic"}, QStyle::SP_DialogSaveButton}},
        {"document-open-symbolic", {{"document-open", "document-open-symbolic"}, QStyle::SP_DialogOpenButton}},
        {"folder-open-symbolic", {{"folder-open", "folder-open-symbolic"}, QStyle::SP_DirOpenIcon}},
        {"changes-prevent-symbolic", {{"object-locked", "changes-prevent", "changes-prevent-symbolic"}}},
        {"changes-allow-symbolic", {{"object-unlocked", "changes-allow", "changes-allow-symbolic"}}},
        {"text-x-generic-symbolic", {{"text-x-generic-symbolic", "text-x-generic"}, QStyle::SP_FileIcon}},
        {"folder-symbolic", {{"folder-symbolic", "folder"}, QStyle::SP_DirIcon}},
        {"dialog-warning-symbolic", {{"dialog-warning", "dialog-warning-symbolic"}, QStyle::SP_MessageBoxWarning}},
        {"dialog-error-symbolic", {{"dialog-error", "dialog-error-symbolic"}, QStyle::SP_MessageBoxCritical}},
        {"dialog-information-symbolic",
         {{"dialog-information", "dialog-information-symbolic"}, QStyle::SP_MessageBoxInformation}},
        {"edit-find-replace-symbolic", {{"edit-find-replace", "edit-find-replace-symbolic"}}},
        {"edit-find-symbolic", {{"edit-find", "edit-find-symbolic"}}},
        {"view-wrapped-symbolic", {{"view-wrapped-symbolic", "view-refresh"}, QStyle::SP_BrowserReload}},
        {"pan-down-symbolic", {{"pan-down-symbolic", "arrow-down", "go-down"}, QStyle::SP_ArrowDown}},
        {"document-new-symbolic", {{"document-new", "document-new-symbolic"}, QStyle::SP_FileIcon}},
        {"folder-new-symbolic", {{"folder-new", "folder-new-symbolic"}, QStyle::SP_FileDialogNewFolder}},
        {"appointment-new-symbolic", {{"appointment-new-symbolic", "appointment-new"}}},
        {"checkbox-checked-symbolic", {{"checkbox-checked-symbolic", "checkbox", "dialog-ok"}, QStyle::SP_DialogApplyButton}},
        {"symbolic-link-symbolic", {{"emblem-symbolic-link", "symbolic-link-symbolic"}, QStyle::SP_FileLinkIcon}},
        {"edit-clear-symbolic", {{"edit-clear", "edit-clear-symbolic"}, QStyle::SP_LineEditClearButton}},
        {"process-stop-symbolic", {{"process-stop", "process-stop-symbolic"}, QStyle::SP_BrowserStop}},
        {"view-refresh-symbolic", {{"view-refresh", "view-refresh-symbolic"}, QStyle::SP_BrowserReload}},
        // Version control actions; Meld's own push and pull icons are used
        // if the theme has none
        {"vc-pull-symbolic", {{"vcs-pull", "vc-pull-symbolic"}}},
        {"vc-push-symbolic", {{"vcs-push", "vc-push-symbolic"}}},
        {"list-add-symbolic", {{"list-add", "list-add-symbolic"}}},
        {"list-remove-symbolic", {{"list-remove", "list-remove-symbolic"}}},
        {"edit-undo-symbolic", {{"edit-undo", "edit-undo-symbolic"}}},
        {"document-revert-symbolic", {{"document-revert", "document-revert-symbolic"}}},
        {"emblem-ok-symbolic", {{"emblem-ok-symbolic", "emblem-ok", "dialog-ok"}, QStyle::SP_DialogApplyButton}},
    };
    return specs;
}

/*!
 * Icons drawn in the text colour, as GTK draws symbolic icons. They're drawn
 * when they're shown, so they follow palette changes, e.g., to the dark
 * style.
 */
class SymbolicIconEngine final : public QIconEngine {
public:
    using Painter = std::function<void(QPainter &, const QRectF &, const QColor &)>;

    explicit SymbolicIconEngine(Painter paint)
        : paint_(std::move(paint))
    {
    }

    void paint(QPainter *painter, const QRect &rect, QIcon::Mode mode, QIcon::State) override
    {
        const QPalette palette = QApplication::palette();
        QColor color = palette.color(QPalette::Active, QPalette::WindowText);
        if (mode == QIcon::Disabled)
            color = palette.color(QPalette::Disabled, QPalette::WindowText);
        else if (mode == QIcon::Selected)
            color = palette.color(QPalette::Active, QPalette::HighlightedText);
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        paint_(*painter, rect, color);
        painter->restore();
    }

    QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override
    {
        return scaledPixmap(size, mode, state, 1.0);
    }

    QPixmap scaledPixmap(const QSize &size, QIcon::Mode mode, QIcon::State state, qreal scale) override
    {
        QPixmap pixmap(size * scale);
        pixmap.setDevicePixelRatio(scale);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        paint(&painter, QRect(QPoint(0, 0), size), mode, state);
        return pixmap;
    }

    QIconEngine *clone() const override { return new SymbolicIconEngine(paint_); }
    QString key() const override { return QStringLiteral("meld-symbolic"); }

private:
    Painter paint_;
};

/*!
 * Meld's own icons, which have variants for dark backgrounds ("-dark"). The
 * comparison's colours choose between them when they're shown, as the
 * change action icons are shown on them.
 */
class VariantIconEngine final : public QIconEngine {
public:
    VariantIconEngine(const QIcon &light, const QIcon &dark)
        : light_(light)
        , dark_(dark)
    {
    }

    const QIcon &current() const { return currentTheme().dark ? dark_ : light_; }

    void paint(QPainter *painter, const QRect &rect, QIcon::Mode mode, QIcon::State state) override
    {
        current().paint(painter, rect, Qt::AlignCenter, mode, state);
    }

    QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override
    {
        return current().pixmap(size, mode, state);
    }

    QPixmap scaledPixmap(const QSize &size, QIcon::Mode mode, QIcon::State state, qreal scale) override
    {
        return current().pixmap(size, scale, mode, state);
    }

    QSize actualSize(const QSize &size, QIcon::Mode mode, QIcon::State state) override
    {
        return current().actualSize(size, mode, state);
    }

    QIconEngine *clone() const override { return new VariantIconEngine(light_, dark_); }
    QString key() const override { return QStringLiteral("meld-variant"); }

private:
    QIcon light_;
    QIcon dark_;
};

//! Simple vector fallbacks for icons that Qt has no standard version of
void drawIcon(const QString &name, QPainter &painter, const QColor &color)
{
    painter.setPen(QPen(color, 3, Qt::SolidLine, Qt::RoundCap));
    painter.setBrush(Qt::NoBrush);
    if (name == QLatin1String("open-menu-symbolic")) {
        for (int y : {9, 16, 23})
            painter.drawLine(7, y, 25, y);
    } else if (name == QLatin1String("edit-copy-symbolic")) {
        painter.setPen(QPen(color, 2.5));
        painter.drawRoundedRect(QRectF(10, 10, 15, 17), 2, 2);
        painter.drawPolyline(QPolygonF({QPointF(7, 21), QPointF(7, 5), QPointF(20, 5)}));
    } else if (name == QLatin1String("changes-prevent-symbolic") || name == QLatin1String("changes-allow-symbolic")) {
        painter.setPen(QPen(color, 2.5));
        painter.drawRoundedRect(QRectF(7, 14, 18, 13), 2, 2);
        QPainterPath shackle;
        shackle.moveTo(11, 14);
        shackle.lineTo(11, 10);
        shackle.arcTo(QRectF(11, 4, 10, 10), 180, -180);
        if (name == QLatin1String("changes-prevent-symbolic"))
            shackle.lineTo(21, 14);
        painter.drawPath(shackle);
    } else if (name == QLatin1String("edit-find-replace-symbolic") || name == QLatin1String("edit-find-symbolic")) {
        painter.drawEllipse(QRectF(6, 6, 14, 14));
        painter.drawLine(18, 18, 26, 26);
    } else if (name == QLatin1String("list-add-symbolic") || name == QLatin1String("list-remove-symbolic")) {
        painter.drawLine(8, 16, 24, 16);
        if (name == QLatin1String("list-add-symbolic"))
            painter.drawLine(16, 8, 16, 24);
    } else if (name == QLatin1String("edit-undo-symbolic") || name == QLatin1String("document-revert-symbolic")) {
        painter.drawArc(QRectF(8, 8, 17, 16), 90 * 16, -250 * 16);
        painter.drawPolyline(QPolygonF({QPointF(13, 4), QPointF(8, 8), QPointF(13, 12)}));
    } else if (name == QLatin1String("appointment-new-symbolic")) {
        painter.drawEllipse(QRectF(5, 5, 22, 22));
        painter.drawLine(16, 16, 16, 9);
        painter.drawLine(16, 16, 21, 19);
    } else {
        painter.drawEllipse(QRectF(8, 8, 16, 16));
    }
}

QIcon drawnIcon(const QString &name)
{
    return QIcon(new SymbolicIconEngine([name](QPainter &painter, const QRectF &rect, const QColor &color) {
        // Drawn on a 32 by 32 grid
        painter.translate(rect.topLeft());
        painter.scale(rect.width() / 32, rect.height() / 32);
        drawIcon(name, painter, color);
    }));
}

//! A symbolic icon, recoloured to the text colour as GTK does
QIcon symbolicIcon(const QString &file)
{
    const QIcon base(file);
    return QIcon(new SymbolicIconEngine([base](QPainter &painter, const QRectF &rect, const QColor &color) {
        const qreal ratio = painter.device() ? painter.device()->devicePixelRatioF() : 1.0;
        QPixmap pixmap = base.pixmap(rect.size().toSize(), ratio);
        if (pixmap.isNull())
            return;
        QPainter tint(&pixmap);
        tint.setCompositionMode(QPainter::CompositionMode_SourceIn);
        tint.fillRect(QRect(QPoint(0, 0), pixmap.size()), color);
        tint.end();
        painter.drawPixmap(rect.topLeft(), pixmap);
    }));
}

} // namespace

namespace {

//! Icon themes with light and dark variants: icons for light backgrounds
//! first
const std::pair<const char *, const char *> ThemeVariants[] = {
    {"breeze", "breeze-dark"},
    {"Papirus", "Papirus-Dark"},
};

bool themeExists(const QString &theme)
{
    for (const QString &path : QIcon::themeSearchPaths()) {
        if (QDir(path).exists(theme + QStringLiteral("/index.theme")))
            return true;
    }
    return false;
}

} // namespace

void updateIconTheme()
{
    // Icons of Qt's own theme loader aren't recoloured for the palette, as
    // KDE's are; their dark variants are for dark palettes
    const bool dark = QApplication::palette().color(QPalette::Window).lightness() < 128;
    const QString current = QIcon::themeName();
    for (const auto &[light, darkVariant] : ThemeVariants) {
        if (current != QLatin1String(light) && current != QLatin1String(darkVariant))
            continue;
        const QString wanted = QLatin1String(dark ? darkVariant : light);
        if (wanted != current && themeExists(wanted))
            QIcon::setThemeName(wanted);
        return;
    }
}

void initialiseIconTheme()
{
#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
    // Platforms without a desktop theme (e.g., offscreen) don't look for
    // icon themes in the standard locations
    QStringList searchPaths = QIcon::themeSearchPaths();
    for (const QString &path : QStandardPaths::locateAll(QStandardPaths::GenericDataLocation, QStringLiteral("icons"),
                                                         QStandardPaths::LocateDirectory)) {
        if (!searchPaths.contains(path))
            searchPaths.append(path);
    }
    QIcon::setThemeSearchPaths(searchPaths);
#endif

    if (!QIcon::themeName().isEmpty() && QIcon::themeName() != QLatin1String("hicolor"))
        return;

    const bool dark = QApplication::palette().color(QPalette::Window).lightness() < 128;
    const QStringList candidates = dark ? QStringList{"breeze-dark", "breeze", "Adwaita", "Papirus-Dark"}
                                        : QStringList{"breeze", "Adwaita", "Papirus", "breeze-dark"};
    for (const QString &theme : candidates) {
        for (const QString &path : QIcon::themeSearchPaths()) {
            if (QDir(path).exists(theme + QStringLiteral("/index.theme"))) {
                QIcon::setThemeName(theme);
                return;
            }
        }
    }
}

QIcon icon(const QString &name)
{
    static QHash<QString, QIcon> cache;
    if (auto it = cache.constFind(name); it != cache.constEnd())
        return *it;

    QIcon result;
    if (name.startsWith(QLatin1String("meld-"))) {
        const QIcon light(QStringLiteral(":/icons/%1.png").arg(name));
        const QString dark = QStringLiteral(":/icons/%1-dark.png").arg(name);
        result = QFile::exists(dark) ? QIcon(new VariantIconEngine(light, QIcon(dark))) : light;
    } else if (const auto it = iconSpecs().constFind(name); it != iconSpecs().constEnd()) {
        for (const QString &themeName : it->themeNames) {
            if (QIcon::hasThemeIcon(themeName)) {
                result = QIcon::fromTheme(themeName);
                break;
            }
        }
        if (result.isNull() && QFile::exists(QStringLiteral(":/icons/%1.svg").arg(name)))
            result = symbolicIcon(QStringLiteral(":/icons/%1.svg").arg(name));
        if (result.isNull() && it->standard != QStyle::SP_CustomBase)
            result = QApplication::style()->standardIcon(it->standard);
        if (result.isNull())
            result = drawnIcon(name);
    } else {
        result = QIcon::fromTheme(name);
    }
    cache.insert(name, result);
    return result;
}

QPixmap tintedPixmap(const QString &name, const QColor &color, int size, qreal devicePixelRatio)
{
    static QHash<QString, QPixmap> cache;
    const QString key = QStringLiteral("%1|%2|%3|%4").arg(name, color.name(QColor::HexArgb)).arg(size).arg(devicePixelRatio);
    if (auto it = cache.constFind(key); it != cache.constEnd())
        return *it;

    QPixmap base = icon(name).pixmap(QSize(size, size), devicePixelRatio);
    if (base.isNull())
        return base;
    QPixmap tinted(base.size());
    tinted.setDevicePixelRatio(base.devicePixelRatio());
    tinted.fill(Qt::transparent);
    QPainter painter(&tinted);
    painter.drawPixmap(0, 0, base);
    painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
    painter.fillRect(QRect(QPoint(0, 0), base.size()), color);
    painter.end();
    cache.insert(key, tinted);
    return tinted;
}

} // namespace meld::icons
