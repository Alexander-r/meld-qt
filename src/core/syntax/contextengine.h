#pragma once

#include <QHash>
#include <QList>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace meld::syntax {

/*!
 * A regular expression from a language definition (GtkSourceRegex)
 *
 * End patterns may contain "\%{name@start}" references to sub-patterns of
 * the start match; these are unresolved until a context starts.
 */
class SourceRegex {
public:
    //! Returns nullptr and sets `error` if the pattern is invalid
    static std::shared_ptr<SourceRegex> create(const QString &pattern, bool anchored, QString *error = nullptr);

    bool isResolved() const { return resolved_; }
    const QString &pattern() const { return pattern_; }

    //! Match at `offset` (anchored regexes) or after it
    QRegularExpressionMatch match(QStringView text, qsizetype offset) const;
    //! The first group with this name that matched, for patterns with
    //! duplicate names
    int groupForName(const QRegularExpressionMatch &match, const QString &name) const;

    //! Replace the "\%{...@start}" references with the escaped text of the
    //! start match's sub-patterns
    std::shared_ptr<SourceRegex> resolve(const SourceRegex &start, const QRegularExpressionMatch &startMatch) const;

private:
    QString pattern_;
    bool resolved_ = true;
    bool anchored_ = false;
    QRegularExpression regex_;
};

enum ContextFlag : unsigned {
    ExtendParent = 1 << 0,
    EndParent = 1 << 1,
    EndAtLineEnd = 1 << 2,
    FirstLineOnly = 1 << 3,
    OnceOnly = 1 << 4,
    StyleInside = 1 << 5,
};

enum class SubPatternWhere {
    Default,
    Start,
    End,
};

struct ContextDefinition;

struct SubPatternDefinition {
    //! Style id, or empty for none
    QString style;
    SubPatternWhere where = SubPatternWhere::Default;
    int number = -1;
    QString name;
};

struct DefinitionChild {
    //! The referenced id, until references are resolved
    QString id;
    ContextDefinition *definition = nullptr;
    QString style;
    //! Whether this refers to all the children of the definition
    bool isRefAll = false;
    //! Whether `style` replaces the definition's style
    bool overrideStyle = false;
    //! Whether the style is ignored for this and all child contexts
    bool overrideStyleDeep = false;
};

struct ContextDefinition {
    enum class Type {
        Simple,
        Container,
    };

    QString id;
    Type type = Type::Container;
    std::shared_ptr<SourceRegex> match;
    std::shared_ptr<SourceRegex> start;
    std::shared_ptr<SourceRegex> end;
    QString defaultStyle;
    std::vector<DefinitionChild> children;
    std::vector<SubPatternDefinition> subPatterns;
    unsigned flags = ExtendParent;

    //! Children, with references to all children of a context expanded
    const std::vector<const DefinitionChild *> &expandedChildren() const;

private:
    mutable std::optional<std::vector<const DefinitionChild *>> expanded_;
};

struct StyleInfo {
    QString name;
    //! The style used if a scheme doesn't define this one
    QString mapTo;
};

class Context;

/*!
 * The definitions for highlighting one language: its contexts, and the
 * contexts of all the languages it refers to (GtkSourceContextData)
 */
class ContextData {
public:
    explicit ContextData(const QString &languageId);
    ~ContextData();

    ContextData(const ContextData &) = delete;
    ContextData &operator=(const ContextData &) = delete;

    QString languageId() const { return languageId_; }
    const ContextDefinition *mainDefinition() const { return main_; }
    ContextDefinition *lookup(const QString &id) const { return definitions_.value(id); }

    //! All styles defined by the loaded languages
    const QHash<QString, StyleInfo> &styles() const { return styles_; }
    //! The map-to style of a style, or an empty string
    QString styleFallback(const QString &styleId) const { return styles_.value(styleId).mapTo; }

    //! Style ids are numbered for compact highlighting results
    int styleIndex(const QString &styleId) const;
    QString styleName(int index) const { return styleNames_.value(index); }
    int styleCount() const { return int(styleNames_.size()); }

    //! The root context, from which highlighting starts
    const Context *rootContext() const;

private:
    friend class LanguageParser;

    QString languageId_;
    QHash<QString, ContextDefinition *> definitions_;
    std::vector<std::unique_ptr<ContextDefinition>> storage_;
    QHash<QString, StyleInfo> styles_;
    ContextDefinition *main_ = nullptr;

    mutable QHash<QString, int> styleIndices_;
    mutable QStringList styleNames_;
    mutable std::unique_ptr<Context> root_;
};

/*!
 * A context at highlighting time (GtkSourceContextEngine's Context)
 *
 * Contexts are created on demand for each definition within a parent, and
 * for each start match text if their end refers to the start match. They
 * are owned by their parent and live as long as the context data.
 */
class Context {
public:
    const ContextDefinition *definition() const { return definition_; }
    const Context *parent() const { return parent_; }
    //! The end regex, with references to the start match resolved
    const SourceRegex *end() const { return end_.get(); }
    //! Style index, or -1 for none
    int style() const { return style_; }

    bool ancestorCanEnd() const;
    bool extendsParent() const;
    bool endsParent() const;
    bool endsAtLineEnd() const;

    //! The regex for finding everything that could happen next in this
    //! context; null if it couldn't be compiled
    const SourceRegex *regexAll() const;
    const Context *child(const DefinitionChild &child, const SourceRegex *start,
                         const QRegularExpressionMatch &startMatch, const ContextData &data) const;

private:
    friend class ContextData;
    Context() = default;

    const ContextDefinition *definition_ = nullptr;
    const Context *parent_ = nullptr;
    std::shared_ptr<SourceRegex> end_;
    int style_ = -1;
    bool allAncestorsExtend_ = false;
    bool ignoreChildrenStyle_ = false;

    mutable std::optional<std::shared_ptr<SourceRegex>> regexAll_;
    struct Children {
        std::unique_ptr<Context> fixed;
        std::map<QString, std::unique_ptr<Context>> byStartText;
    };
    mutable std::map<const ContextDefinition *, Children> children_;
};

/*!
 * The highlighting state between lines: the open contexts
 *
 * States are immutable, and compared to find where re-highlighting after
 * an edit can stop.
 */
class LineState {
public:
    struct Frame {
        const Context *context = nullptr;
        //! Once-only children that already occurred in this context
        QList<const ContextDefinition *> onceOnly;

        friend bool operator==(const Frame &, const Frame &) = default;
    };

    std::vector<Frame> frames;

    friend bool operator==(const LineState &, const LineState &) = default;
};

using LineStatePtr = std::shared_ptr<const LineState>;

//! A highlighted range of a line
struct StyleSpan {
    int start = 0;
    int end = 0;
    int style = -1;
    //! Spans with higher priorities are drawn over lower ones
    int priority = 0;
};

class LineHighlighter {
public:
    explicit LineHighlighter(std::shared_ptr<const ContextData> data);

    const ContextData &data() const { return *data_; }
    LineStatePtr initialState() const { return initial_; }

    /*!
     * Highlight one line (without its terminator), starting from `state`
     *
     * Returns the state for the next line, or nullptr if analysing the line
     * took too long, in which case highlighting should be disabled as
     * GtkSourceView does.
     */
    LineStatePtr highlightLine(QStringView text, bool firstLine, const LineStatePtr &state,
                               std::vector<StyleSpan> &spans) const;

    //! Maximum time for one line before giving up, in milliseconds
    static constexpr int MaxTimeForOneLine = 2000;

private:
    std::shared_ptr<const ContextData> data_;
    LineStatePtr initial_;
};

} // namespace meld::syntax
