#pragma once

#include <QColor>
#include <QHash>
#include <QString>
#include <QStringList>

#include <memory>
#include <optional>
#include <vector>

namespace meld::syntax {

//! Text attributes for a style; unset attributes are inherited from the
//! enclosing text
struct Style {
    std::optional<QColor> foreground;
    std::optional<QColor> background;
    std::optional<QColor> lineBackground;
    std::optional<bool> bold;
    std::optional<bool> italic;
    std::optional<bool> strikethrough;
    //! Pango underline values: 0 none, 1 single, 2 double, 3 low, 4 error,
    //! and 5-7 the "-line" variants of single, double and error
    std::optional<int> underline;
    std::optional<QColor> underlineColor;
    //! Font size factor, e.g., for headings
    std::optional<double> scale;

    bool isEmpty() const
    {
        return !foreground && !background && !lineBackground && !bold && !italic && !strikethrough && !underline
            && !underlineColor && !scale;
    }

    friend bool operator==(const Style &, const Style &) = default;
};

/*!
 * Parse a colour as gdk_rgba_parse() does: X11/CSS colour names, "#rgb",
 * "#rrggbb" (and the 12 and 16 bit forms), "#rgba", "#rrggbbaa" and CSS
 * rgb() and rgba() functions.
 */
std::optional<QColor> parseColor(QStringView text);

//! A style's font scale: Pango's size names (e.g., "x-large") or a positive
//! number, as gtk_source_style_apply() interprets it
std::optional<double> parseScale(QStringView text);

class StyleScheme {
public:
    //! Load a scheme; returns nullptr and sets `error` if it's invalid
    static std::unique_ptr<StyleScheme> fromFile(const QString &fileName, QString *error = nullptr);
    static std::unique_ptr<StyleScheme> fromData(const QByteArray &data, const QString &fileName,
                                                 QString *error = nullptr);

    QString id() const { return id_; }
    QString name() const { return name_; }
    QString description() const { return description_; }
    QStringList authors() const { return authors_; }
    QString fileName() const { return fileName_; }
    QString parentId() const { return parentId_; }
    const StyleScheme *parent() const { return parent_; }
    void setParent(const StyleScheme *parent);
    //! Metadata properties (e.g., "variant" and "dark-variant" in newer
    //! schemes)
    QString metadata(const QString &name) const { return metadata_.value(name); }

    /*!
     * The style with this id, from this scheme or its parents
     *
     * As in GtkSourceView, "def:net-address" falls back to the older
     * "def:underlined" style.
     */
    std::optional<Style> style(const QString &styleId) const;

private:
    StyleScheme() = default;

    //! A style as written in the file, with colours not yet resolved
    struct RawStyle {
        QString foreground;
        QString background;
        QString lineBackground;
        QString underlineColor;
        std::optional<bool> bold;
        std::optional<bool> italic;
        std::optional<bool> strikethrough;
        std::optional<int> underline;
        std::optional<double> scale;
    };

    std::optional<Style> styleInternal(const QString &styleId) const;
    //! get_color_by_name(): the colour string a name refers to
    QString colorByName(const QString &name) const;
    std::optional<QColor> resolveColor(const QString &name) const;

    QString id_;
    QString name_;
    QString description_;
    QStringList authors_;
    QString fileName_;
    QString parentId_;
    QHash<QString, QString> metadata_;
    QHash<QString, RawStyle> styles_;
    QHash<QString, QString> namedColors_;
    const StyleScheme *parent_ = nullptr;
    mutable QHash<QString, std::optional<Style>> cache_;
};

class StyleSchemeManager {
public:
    //! The bundled schemes, and Meld's own
    static StyleSchemeManager &instance();
    //! Load the schemes (*.xml) in the given folders; earlier folders take
    //! precedence for schemes with the same id
    explicit StyleSchemeManager(const QStringList &searchPath);
    ~StyleSchemeManager();

    StyleSchemeManager(const StyleSchemeManager &) = delete;
    StyleSchemeManager &operator=(const StyleSchemeManager &) = delete;

    //! Scheme ids, sorted by scheme name
    QStringList schemeIds() const { return ids_; }
    const StyleScheme *scheme(const QString &id) const;

private:
    std::vector<std::unique_ptr<StyleScheme>> schemes_;
    QHash<QString, StyleScheme *> byId_;
    QStringList ids_;
};

} // namespace meld::syntax
