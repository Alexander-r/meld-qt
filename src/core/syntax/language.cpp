// Qt port of GtkSourceView's languages and language manager
// (gtksourcelanguage.c, gtksourcelanguagemanager.c and
// gtksourcelanguage-parser-2.c)

#include "core/syntax/language.h"

#include "core/syntax/contextengine.h"

#include <QCollator>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QMimeDatabase>
#include <QRegularExpression>
#include <QSet>
#include <QXmlStreamReader>

#include <algorithm>

namespace meld::syntax {

Q_LOGGING_CATEGORY(lcLanguage, "meld.syntax.language")

namespace {

//! Boolean attributes of the <language> element (string_to_bool())
bool stringToBool(QStringView value)
{
    return value.compare(u"yes", Qt::CaseInsensitive) == 0 || value.compare(u"true", Qt::CaseInsensitive) == 0
        || value == u"1";
}

//! Boolean attributes in language definitions, which are only true if they
//! say so (str_to_bool())
bool isTrue(QStringView value)
{
    return value.compare(u"true", Qt::CaseInsensitive) == 0;
}

//! g_ascii_strdown()
QString asciiLower(const QString &string)
{
    QString lower = string;
    for (QChar &c : lower) {
        if (c.unicode() < 0x80)
            c = c.toLower();
    }
    return lower;
}

// ---------------------------------------------------------------------------
// A minimal XML tree, standing in for the libxml2 trees the original
// parser expands context elements to

struct XmlNode {
    enum class Kind {
        Element,
        Text,
        Comment,
    };

    Kind kind = Kind::Element;
    QString name;
    QXmlStreamAttributes attributes;
    QString text;
    bool cdata = false;
    std::vector<std::unique_ptr<XmlNode>> children;

    QString attribute(const char *attributeName) const { return attributes.value(QLatin1String(attributeName)).toString(); }
    bool hasAttribute(const char *attributeName) const { return attributes.hasAttribute(QLatin1String(attributeName)); }
    bool hasElementChildren() const
    {
        return std::any_of(children.begin(), children.end(),
                           [](const auto &child) { return child->kind == Kind::Element; });
    }
};

//! Whitespace between elements is ignorable, as with xmlKeepBlanksDefault(0)
void dropBlanks(XmlNode &node)
{
    if (node.hasElementChildren()) {
        std::erase_if(node.children, [](const std::unique_ptr<XmlNode> &child) {
            return child->kind == XmlNode::Kind::Text && !child->cdata && child->text.trimmed().isEmpty();
        });
    }
    for (const auto &child : node.children)
        dropBlanks(*child);
}

std::unique_ptr<XmlNode> parseXml(const QByteArray &data, QString *error)
{
    auto document = std::make_unique<XmlNode>();
    std::vector<XmlNode *> stack{document.get()};
    QXmlStreamReader xml(data);
    while (!xml.atEnd()) {
        switch (xml.readNext()) {
        case QXmlStreamReader::StartElement: {
            auto node = std::make_unique<XmlNode>();
            node->name = xml.name().toString();
            node->attributes = xml.attributes();
            XmlNode *raw = node.get();
            stack.back()->children.push_back(std::move(node));
            stack.push_back(raw);
            break;
        }
        case QXmlStreamReader::EndElement:
            stack.pop_back();
            break;
        case QXmlStreamReader::Characters: {
            XmlNode *parent = stack.back();
            if (!xml.isCDATA() && !parent->children.empty() && parent->children.back()->kind == XmlNode::Kind::Text
                && !parent->children.back()->cdata) {
                parent->children.back()->text += xml.text();
            } else {
                auto node = std::make_unique<XmlNode>();
                node->kind = XmlNode::Kind::Text;
                node->text = xml.text().toString();
                node->cdata = xml.isCDATA();
                parent->children.push_back(std::move(node));
            }
            break;
        }
        case QXmlStreamReader::Comment: {
            auto node = std::make_unique<XmlNode>();
            node->kind = XmlNode::Kind::Comment;
            node->text = xml.text().toString();
            stack.back()->children.push_back(std::move(node));
            break;
        }
        default:
            break;
        }
    }
    if (xml.hasError()) {
        if (error)
            *error = QStringLiteral("line %1: %2").arg(xml.lineNumber()).arg(xml.errorString());
        return nullptr;
    }
    dropBlanks(*document);
    return document;
}

//! The content of an element's first child node (child->children->content)
std::optional<QString> firstChildContent(const XmlNode &node)
{
    if (node.children.empty())
        return std::nullopt;
    const XmlNode &first = *node.children.front();
    if (first.kind == XmlNode::Kind::Element)
        return std::nullopt;
    return first.text;
}

//! xmlNodeGetContent(): all the text in an element
QString nodeContent(const XmlNode &node)
{
    QString content;
    for (const auto &child : node.children) {
        if (child->kind == XmlNode::Kind::Text)
            content += child->text;
        else if (child->kind == XmlNode::Kind::Element)
            content += nodeContent(*child);
    }
    return content;
}

QByteArray readFile(const QString &fileName, QString *error)
{
    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("unable to open the file");
        return {};
    }
    return file.readAll();
}

} // namespace

// ---------------------------------------------------------------------------
// The language definition parser (gtksourcelanguage-parser-2.c)

class LanguageParser {
public:
    LanguageParser(const LanguageManager &manager, ContextData &data)
        : manager_(manager)
        , data_(data)
    {
    }

    bool parse(const Language &language)
    {
        rootDecoration_ = language.id() + QLatin1Char(':');
        if (!parseFile(language.fileName()))
            return false;
        return finishParse();
    }

    QString error() const { return error_; }

private:
    enum RegexFlag : unsigned {
        Caseless = 1 << 0,
        Extended = 1 << 1,
        DupNames = 1 << 2,
    };

    //! The state for parsing one file
    struct FileState {
        QString currentLangId;
        QStringList currParents;
        unsigned idCookie = 0;
        unsigned regexFlags = 0;
        QString openingDelimiter = QStringLiteral("\\b");
        QString closingDelimiter = QStringLiteral("\\b");
    };

    bool fail(const QString &message)
    {
        if (error_.isEmpty())
            error_ = message;
        return false;
    }

    //! Whether an id has a language prefix; "lang:*" doesn't
    static bool idIsDecorated(const QString &id, QString *langId = nullptr)
    {
        const qsizetype colon = id.indexOf(QLatin1Char(':'));
        if (colon < 0 || QStringView(id).sliced(colon + 1) == QLatin1String("*"))
            return false;
        if (langId)
            *langId = id.left(colon);
        return true;
    }

    QString decorate(const FileState &state, const QString &id) const { return state.currentLangId + QLatin1Char(':') + id; }

    QString decorateIfNeeded(const FileState &state, const QString &id) const
    {
        return idIsDecorated(id) ? id : decorate(state, id);
    }

    bool parseFile(const QString &fileName)
    {
        QString error;
        const QByteArray contents = readFile(fileName, &error);
        if (contents.isEmpty() && !error.isEmpty())
            return fail(QStringLiteral("%1: %2").arg(fileName, error));
        const std::unique_ptr<XmlNode> document = parseXml(contents, &error);
        if (!document)
            return fail(QStringLiteral("in file %1: %2").arg(fileName, error));

        FileState state;
        for (const auto &child : document->children) {
            if (child->kind == XmlNode::Kind::Element && !visit(state, *child))
                return false;
        }
        return true;
    }

    bool visit(FileState &state, const XmlNode &node)
    {
        const QString &name = node.name;
        bool visitChildren = true;
        bool ok = true;
        bool pushed = false;
        if (name == QLatin1String("context")) {
            ok = handleContext(state, node, &pushed);
        } else if (name == QLatin1String("replace")) {
            handleReplace(state, node);
        } else if (name == QLatin1String("define-regex")) {
            ok = handleDefineRegex(state, node);
            visitChildren = false;
        } else if (name == QLatin1String("language")) {
            ok = handleLanguage(state, node);
        } else if (name == QLatin1String("styles")) {
            ok = handleStyles(state, node);
            visitChildren = false;
        } else if (name == QLatin1String("keyword-char-class")) {
            handleKeywordCharClass(state, node);
            visitChildren = false;
        } else if (name == QLatin1String("default-regex-options")) {
            state.regexFlags = regexFlags(node, 0);
        }
        if (!ok)
            return false;

        if (visitChildren) {
            for (const auto &child : node.children) {
                if (child->kind == XmlNode::Kind::Element && !visit(state, *child))
                    return false;
            }
        }
        if (pushed)
            state.currParents.removeFirst();
        return true;
    }

    static unsigned updateRegexFlags(unsigned flags, QStringView option, QStringView value)
    {
        bool set = isTrue(value);
        unsigned flag = 0;
        if (option == QLatin1String("case-sensitive")) {
            flag = Caseless;
            set = !set;
        } else if (option == QLatin1String("extended")) {
            flag = Extended;
        } else if (option == QLatin1String("dupnames")) {
            flag = DupNames;
        } else {
            return flags;
        }
        return set ? flags | flag : flags & ~flag;
    }

    static unsigned regexFlags(const XmlNode &node, unsigned flags)
    {
        for (const QXmlStreamAttribute &attribute : node.attributes)
            flags = updateRegexFlags(flags, attribute.name(), attribute.value());
        return flags;
    }

    //! expand_regex(): resolve \%{id} and \%[ \%], and set the regex flags
    bool expandRegex(const FileState &state, const QString &regex, unsigned flags, bool insertParentheses,
                     QString *result)
    {
        // \%{id} references to defined regexes; references with "@" are to
        // sub-patterns of the start regex and are resolved by the engine
        static const QRegularExpression varRe(QStringLiteral(R"((?<!\\)(\\\\)*\\%\{([^@]*?)\})"));
        QString expanded;
        qsizetype last = 0;
        for (auto it = varRe.globalMatch(regex); it.hasNext();) {
            const QRegularExpressionMatch m = it.next();
            const QString id = decorateIfNeeded(state, m.captured(2).trimmed());
            const auto subst = definedRegexes_.constFind(id);
            if (subst == definedRegexes_.constEnd())
                return fail(QStringLiteral("Unknown id “%1” in regex “%2”").arg(id, regex));
            expanded += QStringView(regex).mid(last, m.capturedStart() - last);
            expanded += m.captured(1) + *subst;
            last = m.capturedEnd();
        }
        expanded += QStringView(regex).mid(last);

        // \%[ and \%] word delimiters
        static const QRegularExpression delimiterRe(QStringLiteral(R"((?<!\\)(\\\\)*\\%(\[|\]))"));
        QString delimited;
        last = 0;
        for (auto it = delimiterRe.globalMatch(expanded); it.hasNext();) {
            const QRegularExpressionMatch m = it.next();
            delimited += QStringView(expanded).mid(last, m.capturedStart() - last);
            delimited += m.captured(1);
            delimited += m.captured(2) == QLatin1String("[") ? state.openingDelimiter : state.closingDelimiter;
            last = m.capturedEnd();
        }
        delimited += QStringView(expanded).mid(last);

        // Set the options, and add non-capturing parentheses for included
        // regular expressions
        QString out;
        if (insertParentheses)
            out += QStringLiteral("(?:");
        out += QStringLiteral("(?");
        if (flags & Caseless)
            out += QLatin1Char('i');
        if (flags & Extended)
            out += QLatin1Char('x');
        // J is added here if it's used, but -J isn't added below
        if (flags & DupNames)
            out += QLatin1Char('J');
        if ((flags & (Caseless | Extended)) != (Caseless | Extended)) {
            out += QLatin1Char('-');
            if (!(flags & Caseless))
                out += QLatin1Char('i');
            if (!(flags & Extended))
                out += QLatin1Char('x');
        }
        out += QLatin1Char(')');
        out += delimited;
        if (insertParentheses) {
            // Otherwise an extended regex ending with a comment would have
            // the ')' in the comment
            if (flags & Extended)
                out += QLatin1Char('\n');
            out += QLatin1Char(')');
        }
        *result = out;
        return true;
    }

    unsigned contextFlags(const XmlNode &node) const
    {
        unsigned flags = ExtendParent;
        const std::pair<const char *, unsigned> names[] = {
            {"extend-parent", ExtendParent}, {"end-parent", EndParent},         {"end-at-line-end", EndAtLineEnd},
            {"first-line-only", FirstLineOnly}, {"once-only", OnceOnly},       {"style-inside", StyleInside},
        };
        for (const auto &[attribute, flag] : names) {
            if (node.hasAttribute(attribute))
                flags = isTrue(node.attribute(attribute)) ? flags | flag : flags & ~flag;
        }
        return flags;
    }

    //! _gtk_source_context_data_define_context()
    bool defineContext(const QString &id, const QString &parentId, const std::optional<QString> &match,
                       const std::optional<QString> &start, const std::optional<QString> &end,
                       const QString &style, unsigned flags)
    {
        if (data_.definitions_.contains(id))
            return fail(QStringLiteral("duplicated context id “%1”").arg(id));
        const bool simple = match.has_value();
        if (simple && (start || end))
            return fail(QStringLiteral("insufficient or redundant arguments creating the context '%1'").arg(id));

        ContextDefinition *parent = nullptr;
        if (!parentId.isEmpty()) {
            parent = data_.definitions_.value(parentId);
            if (!parent)
                return fail(QStringLiteral("invalid parent '%1' for context '%2'").arg(parentId, id));
        }

        auto definition = std::make_unique<ContextDefinition>();
        definition->id = id;
        definition->type = simple ? ContextDefinition::Type::Simple : ContextDefinition::Type::Container;
        definition->defaultStyle = style;
        definition->flags = flags;
        QString error;
        const auto compile = [&](const std::optional<QString> &pattern, std::shared_ptr<SourceRegex> &target,
                                 bool mayRefer) {
            if (!pattern)
                return true;
            target = SourceRegex::create(*pattern, true, &error);
            if (!target)
                return fail(QStringLiteral("in context “%1”: %2").arg(id, error));
            if (!mayRefer && !target->isResolved())
                return fail(QStringLiteral("context “%1” cannot contain a \\%{...@start} command").arg(id));
            return true;
        };
        if (!compile(match, definition->match, false) || !compile(start, definition->start, false)
            || !compile(end, definition->end, true)) {
            return false;
        }

        ContextDefinition *raw = definition.get();
        data_.storage_.push_back(std::move(definition));
        data_.definitions_.insert(id, raw);
        data_.definitions_.insert(QLatin1Char('@') + id, raw);
        if (parent)
            parent->children.push_back(DefinitionChild{id, nullptr, QString(), false, false, false});
        return true;
    }

    bool createDefinition(FileState &state, const XmlNode &node, const QString &id, const QString &parentId,
                          const QString &style)
    {
        std::optional<QString> match;
        std::optional<QString> start;
        std::optional<QString> end;
        std::optional<QString> prefix;
        std::optional<QString> suffix;
        std::optional<QString> keywords;
        unsigned matchFlags = 0;
        unsigned startFlags = 0;
        unsigned endFlags = 0;
        const unsigned flags = contextFlags(node);

        for (const auto &child : node.children) {
            if (child->kind != XmlNode::Kind::Element)
                continue;
            const QString &name = child->name;
            if (name == QLatin1String("match") && !child->children.empty()) {
                match = firstChildContent(*child);
                matchFlags = regexFlags(*child, state.regexFlags);
            } else if (name == QLatin1String("start")) {
                // If the <start> element is present but has no content use
                // an empty string
                start = firstChildContent(*child).value_or(QString());
                startFlags = regexFlags(*child, state.regexFlags);
            } else if (name == QLatin1String("end")) {
                end = firstChildContent(*child).value_or(QString());
                endFlags = regexFlags(*child, state.regexFlags);
            } else if (name == QLatin1String("prefix")) {
                prefix = firstChildContent(*child).value_or(QString());
            } else if (name == QLatin1String("suffix")) {
                suffix = firstChildContent(*child).value_or(QString());
            } else if (name == QLatin1String("keyword") && !child->children.empty()) {
                const QString keyword = firstChildContent(*child).value_or(QString());
                if (!keywords)
                    keywords = prefix.value_or(state.openingDelimiter) + QLatin1Char('(') + keyword;
                else
                    *keywords += QLatin1Char('|') + keyword;
            }
        }
        if (keywords) {
            *keywords += QLatin1Char(')') + suffix.value_or(state.closingDelimiter);
            match = keywords;
            matchFlags = state.regexFlags;
        }

        QString expanded;
        if (start) {
            if (!expandRegex(state, *start, startFlags, false, &expanded))
                return false;
            start = expanded;
        }
        if (end) {
            if (!expandRegex(state, *end, endFlags, false, &expanded))
                return false;
            end = expanded;
        }
        if (match) {
            if (!expandRegex(state, *match, matchFlags, false, &expanded))
                return false;
            match = expanded;
        }
        return defineContext(id, parentId, match, start, end, style, flags);
    }

    bool addRef(FileState &state, const QString &ref, bool ignoreStyle, bool original, const QString &style)
    {
        QString refId;
        QString langId;
        if (idIsDecorated(ref, &langId)) {
            if (!loadedLangIds_.contains(langId) && !loadLanguage(langId, ref))
                return false;
            refId = ref;
        } else {
            refId = decorate(state, ref);
        }

        bool all = false;
        if (ref.endsWith(QLatin1String(":*"))) {
            all = true;
            refId.chop(2);
        }
        const bool overrideStyle = ignoreStyle || !style.isEmpty();
        if (all && overrideStyle) {
            return fail(QStringLiteral("style override used with wildcard context reference in language '%1' in ref '%2'")
                            .arg(state.currentLangId, ref));
        }

        // _gtk_source_context_data_add_ref()
        const QString parentId = state.currParents.value(0);
        ContextDefinition *parent = data_.definitions_.value(parentId);
        if (!parent || parent->type != ContextDefinition::Type::Container)
            return fail(QStringLiteral("invalid parent type for the context '%1'").arg(refId));
        const ContextDefinition *referenced = data_.definitions_.value(refId);
        if (referenced && referenced->type == ContextDefinition::Type::Container && !referenced->start)
            all = true;
        if (all && overrideStyle) {
            return fail(QStringLiteral("style override used with wildcard context reference in language “%1” in ref “%2”")
                            .arg(data_.languageId_, refId));
        }
        DefinitionChild child;
        child.id = original ? QLatin1Char('@') + refId : refId;
        child.style = style;
        child.isRefAll = all;
        child.overrideStyle = overrideStyle;
        child.overrideStyleDeep = overrideStyle && style.isEmpty();
        parent->children.push_back(child);
        return true;
    }

    bool createSubPattern(FileState &state, const XmlNode &node, const QString &id, const QString &subPattern,
                          const QString &style)
    {
        if (data_.definitions_.contains(id))
            return fail(QStringLiteral("duplicated context id “%1”").arg(id));
        ContextDefinition *parent = data_.definitions_.value(state.currParents.value(0));
        if (!parent)
            return fail(QStringLiteral("invalid parent for sub pattern '%1'").arg(id));

        const QString where = node.attribute("where");
        std::optional<SubPatternWhere> whereValue;
        if (where.isEmpty() || where == QLatin1String("default"))
            whereValue = SubPatternWhere::Default;
        else if (where == QLatin1String("start"))
            whereValue = SubPatternWhere::Start;
        else if (where == QLatin1String("end"))
            whereValue = SubPatternWhere::End;
        const bool simple = parent->type == ContextDefinition::Type::Simple;
        if (!whereValue || (simple && *whereValue != SubPatternWhere::Default)
            || (!simple && *whereValue == SubPatternWhere::Default)) {
            return fail(QStringLiteral("invalid location ('%1') for sub pattern '%2'").arg(where, id));
        }

        SubPatternDefinition sp;
        sp.style = style;
        sp.where = *whereValue;
        bool isNumber = false;
        const int number = subPattern.toInt(&isNumber);
        if (isNumber && number >= 0)
            sp.number = number;
        else
            sp.name = subPattern;
        parent->subPatterns.push_back(sp);
        return true;
    }

    bool handleContext(FileState &state, const XmlNode &node, bool *pushed)
    {
        const bool hasRef = node.hasAttribute("ref");
        const QString ref = node.attribute("ref");
        const bool ignoreStyle = node.hasAttribute("ignore-style") && isTrue(node.attribute("ignore-style"));
        QString styleRef;
        if (node.hasAttribute("style-ref"))
            styleRef = decorateIfNeeded(state, node.attribute("style-ref"));

        if (ignoreStyle && !hasRef)
            return fail(QStringLiteral("ignore-style used not in a reference to context"));
        if (ignoreStyle && !styleRef.isEmpty())
            qCWarning(lcLanguage) << "style-ref and ignore-style used simultaneously";

        if (hasRef) {
            const bool original = node.hasAttribute("original") && isTrue(node.attribute("original"));
            // As in the original, a style-ref takes precedence over
            // ignore-style if both are used
            return addRef(state, ref, ignoreStyle, original, styleRef);
        }

        QString id = node.attribute("id");
        if (!node.hasAttribute("id"))
            id = QStringLiteral("unnamed-%1").arg(state.idCookie++);
        id = decorateIfNeeded(state, id);

        if (node.hasAttribute("sub-pattern"))
            return createSubPattern(state, node, id, node.attribute("sub-pattern"), styleRef);

        const QString parentId = state.currParents.value(0);
        const bool isEmpty = node.children.empty();
        bool success = false;
        if (isEmpty)
            success = defineContext(id, parentId, QStringLiteral("$^"), std::nullopt, std::nullopt, QString(), 0);
        else
            success = createDefinition(state, node, id, parentId, styleRef);
        if (success && !isEmpty) {
            // Contexts can be defined inside it
            state.currParents.prepend(id);
            *pushed = true;
        }
        return success;
    }

    void handleReplace(FileState &state, const XmlNode &node)
    {
        // The id is used as it is, but the replacement is decorated
        replacements_.append({node.attribute("id"), decorateIfNeeded(state, node.attribute("ref"))});
    }

    bool handleLanguage(FileState &state, const XmlNode &node)
    {
        if (node.attribute("version") != QLatin1String("2.0"))
            return fail(QStringLiteral("wrong language version '%1', expected '2.0'").arg(node.attribute("version")));
        state.currentLangId = node.attribute("id");
        loadedLangIds_.insert(state.currentLangId);
        return true;
    }

    bool handleDefineRegex(FileState &state, const XmlNode &node)
    {
        const QString id = decorateIfNeeded(state, node.attribute("id"));
        unsigned flags = state.regexFlags;
        for (const char *option : {"extended", "case-sensitive", "dupnames"}) {
            if (node.hasAttribute(option))
                flags = updateRegexFlags(flags, QString::fromLatin1(option), node.attribute(option));
        }
        // The text node following the start tag, as read by the original's
        // XML reader, for which whitespace isn't text
        QString regex;
        if (!node.children.empty()) {
            const XmlNode &first = *node.children.front();
            const bool blank = std::all_of(first.text.begin(), first.text.end(), [](QChar c) {
                return c == QLatin1Char(' ') || c == QLatin1Char('\t') || c == QLatin1Char('\n')
                    || c == QLatin1Char('\r');
            });
            if (first.kind == XmlNode::Kind::Text && (first.cdata || !blank))
                regex = first.text;
        }
        QString expanded;
        if (!expandRegex(state, regex, flags, true, &expanded))
            return false;
        definedRegexes_.insert(id, expanded);
        return true;
    }

    void handleKeywordCharClass(FileState &state, const XmlNode &node)
    {
        const QString charClass = nodeContent(node);
        state.openingDelimiter = QStringLiteral("(?<!%1)(?=%1)").arg(charClass);
        state.closingDelimiter = QStringLiteral("(?<=%1)(?!%1)").arg(charClass);
    }

    bool handleStyles(FileState &state, const XmlNode &node)
    {
        for (const auto &child : node.children) {
            if (child->kind != XmlNode::Kind::Element || child->name != QLatin1String("style"))
                continue;
            const QString id = decorateIfNeeded(state, child->attribute("id"));
            const QString name = child->hasAttribute("_name") ? child->attribute("_name") : child->attribute("name");
            const QString mapTo = child->attribute("map-to");
            QString langId;
            if (!mapTo.isEmpty() && !idIsDecorated(mapTo, &langId)) {
                return fail(
                    QStringLiteral("the map-to attribute '%1' for the style '%2' lacks the prefix").arg(mapTo, id));
            }
            if (!langId.isEmpty() && !loadedLangIds_.contains(langId) && !loadLanguage(langId, mapTo))
                return false;
            // Remember the style name only for styles of the main language
            StyleInfo info;
            if (id.startsWith(rootDecoration_))
                info.name = name;
            info.mapTo = mapTo;
            data_.styles_.insert(id, info);
        }
        return true;
    }

    bool loadLanguage(const QString &langId, const QString &reference)
    {
        const Language *language = manager_.language(langId);
        if (!language)
            return fail(QStringLiteral("unable to resolve language '%1' in ref '%2'").arg(langId, reference));
        return parseFile(language->fileName());
    }

    //! _gtk_source_context_data_finish_parse()
    bool finishParse()
    {
        for (const auto &[id, replaceWith] : std::as_const(replacements_)) {
            if (!data_.definitions_.contains(id))
                return fail(QStringLiteral("unknown context “%1”").arg(id));
            ContextDefinition *replacement = data_.definitions_.value(replaceWith);
            if (!replacement)
                return fail(QStringLiteral("unknown context “%1”").arg(replaceWith));
            data_.definitions_.insert(id, replacement);
        }

        for (const auto &definition : data_.storage_) {
            for (DefinitionChild &child : definition->children) {
                if (child.definition)
                    continue;
                ContextDefinition *ref = data_.definitions_.value(child.id);
                if (!ref)
                    return fail(QStringLiteral("invalid context reference “%1”").arg(child.id));
                child.definition = ref;
                // References to containers without start are references to
                // all their children
                if (ref->type == ContextDefinition::Type::Container && !ref->start) {
                    if (child.overrideStyle) {
                        return fail(QStringLiteral(
                                        "style override used with wildcard context reference in language “%1” in ref “%2”")
                                        .arg(data_.languageId_, ref->id));
                    }
                    child.isRefAll = true;
                }
            }
        }

        data_.main_ = data_.definitions_.value(data_.languageId_ + QLatin1Char(':') + data_.languageId_);
        if (!data_.main_)
            return fail(QStringLiteral("Missing main language definition (id = \"%1\".)").arg(data_.languageId_));
        return true;
    }

    const LanguageManager &manager_;
    ContextData &data_;
    QString rootDecoration_;
    QHash<QString, QString> definedRegexes_;
    QSet<QString> loadedLangIds_;
    QList<std::pair<QString, QString>> replacements_;
    QString error_;
};

// ---------------------------------------------------------------------------
// Language

std::unique_ptr<Language> Language::fromFile(const QString &fileName, QString *error)
{
    const auto failed = [error](const QString &message) {
        if (error)
            *error = message;
        return nullptr;
    };

    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly))
        return failed(file.errorString());
    QXmlStreamReader xml(&file);
    // The first <language> element, wherever it is
    while (!xml.atEnd() && !(xml.readNext() == QXmlStreamReader::StartElement && xml.name() == QLatin1String("language"))) {
    }
    if (xml.tokenType() != QXmlStreamReader::StartElement)
        return failed(QStringLiteral("Failed to parse '%1'").arg(fileName));

    auto language = std::unique_ptr<Language>(new Language);
    language->fileName_ = fileName;
    const QXmlStreamAttributes attributes = xml.attributes();
    const auto attribute = [&attributes](const char *name) -> std::optional<QString> {
        if (!attributes.hasAttribute(QLatin1String(name)))
            return std::nullopt;
        return attributes.value(QLatin1String(name)).toString();
    };

    language->hidden_ = stringToBool(attribute("hidden").value_or(QString()));
    for (const char *property : {"mimetypes", "globs"}) {
        if (const std::optional<QString> value = attribute(property))
            language->properties_.insert(QLatin1String(property), *value);
    }
    const std::optional<QString> name = attribute("_name") ? attribute("_name") : attribute("name");
    if (!name)
        return failed(QStringLiteral("Impossible to get language name from file '%1'").arg(fileName));
    language->name_ = *name;
    language->id_ = asciiLower(attribute("id").value_or(*name));
    language->section_ = attribute("_section").value_or(attribute("section").value_or(QStringLiteral("Others")));

    const std::optional<QString> version = attribute("version");
    if (!version)
        return failed(QStringLiteral("Impossible to get version number from file '%1'").arg(fileName));
    if (*version != QLatin1String("1.0") && *version != QLatin1String("2.0"))
        return failed(QStringLiteral("Unsupported language spec version '%1' in file '%2'").arg(*version, fileName));

    if (*version == QLatin1String("2.0")) {
        // The properties in the first <metadata> element
        while (!xml.atEnd()) {
            if (xml.readNext() != QXmlStreamReader::StartElement || xml.name() != QLatin1String("metadata"))
                continue;
            while (xml.readNextStartElement()) {
                if (xml.name() != QLatin1String("property")) {
                    xml.skipCurrentElement();
                    continue;
                }
                const QXmlStreamAttributes propertyAttributes = xml.attributes();
                const QString content = xml.readElementText(QXmlStreamReader::IncludeChildElements);
                if (propertyAttributes.hasAttribute(QLatin1String("name")))
                    language->properties_.insert(propertyAttributes.value(QLatin1String("name")).toString(), content);
            }
            break;
        }
    }
    return language;
}

QStringList Language::splitProperty(const QString &name) const
{
    const QString value = properties_.value(name);
    if (value.isEmpty())
        return {};
    return value.split(QLatin1Char(';'));
}

// ---------------------------------------------------------------------------
// LanguageManager

LanguageManager &LanguageManager::instance()
{
    static LanguageManager manager({QStringLiteral(":/language-specs")});
    return manager;
}

LanguageManager::LanguageManager(const QStringList &searchPath)
{
    for (const QString &folder : searchPath) {
        const QDir dir(folder);
        for (const QString &entry : dir.entryList({QStringLiteral("*.lang")}, QDir::Files, QDir::Name)) {
            QString error;
            std::unique_ptr<Language> language = Language::fromFile(dir.filePath(entry), &error);
            if (!language) {
                qCWarning(lcLanguage) << "Error reading language specification file" << dir.filePath(entry) << error;
                continue;
            }
            // Earlier folders take precedence
            if (byId_.contains(language->id()))
                continue;
            byId_.insert(language->id(), language.get());
            languages_.push_back(std::move(language));
        }
    }

    // Sort alphabetically by name, ready to use in a list
    std::vector<const Language *> sorted;
    for (const auto &language : languages_)
        sorted.push_back(language.get());
    QCollator collator;
    std::stable_sort(sorted.begin(), sorted.end(), [&collator](const Language *a, const Language *b) {
        return collator.compare(a->name(), b->name()) < 0;
    });
    for (const Language *language : sorted)
        ids_.append(language->id());
}

LanguageManager::~LanguageManager() = default;

const Language *LanguageManager::language(const QString &id) const
{
    return byId_.value(id);
}

namespace {

QMimeDatabase &mimeDatabase()
{
    static QMimeDatabase database;
    return database;
}

//! g_content_type_from_mime_type(): the canonical name of a MIME type,
//! resolving aliases
QString contentTypeFromMimeType(const QString &mimeType)
{
    const QMimeType type = mimeDatabase().mimeTypeForName(mimeType);
    return type.isValid() ? type.name() : mimeType;
}

//! g_content_type_is_a(), as xdgmime's mime type subclass check
bool contentTypeIsA(const QString &type, const QString &supertype)
{
    const QString mime = contentTypeFromMimeType(type);
    const QString base = contentTypeFromMimeType(supertype);
    if (mime == base)
        return true;
    // Media types, e.g., "text/*"
    if (base.endsWith(QLatin1String("/*")) && mime.startsWith(QStringView(base).chopped(1)))
        return true;
    if (base == QLatin1String("text/plain") && mime.startsWith(QLatin1String("text/")))
        return true;
    if (base == QLatin1String("application/octet-stream") && !mime.startsWith(QLatin1String("inode/")))
        return true;
    const QMimeType mimeType = mimeDatabase().mimeTypeForName(mime);
    return mimeType.isValid() && mimeType.inherits(base);
}

} // namespace

const Language *LanguageManager::languageForMimeTypePass(const QString &mimeType, bool exactMatch) const
{
    for (const QString &id : ids_) {
        const Language *language = byId_.value(id);
        for (const QString &type : language->mimeTypes()) {
            if (exactMatch ? mimeType == type : contentTypeIsA(mimeType, type))
                return language;
        }
    }
    return nullptr;
}

const Language *LanguageManager::languageForMimeType(const QString &mimeType) const
{
    if (const Language *language = languageForMimeTypePass(mimeType, true))
        return language;
    return languageForMimeTypePass(mimeType, false);
}

const Language *LanguageManager::guessLanguage(const QString &fileName, const QString &contentType) const
{
    if (fileName.isEmpty() && contentType.isEmpty())
        return nullptr;

    // Languages whose globs match, in order of names
    std::vector<const Language *> languages;
    if (!fileName.isEmpty()) {
        for (const QString &id : ids_) {
            const Language *language = byId_.value(id);
            for (const QString &glob : language->globs()) {
                if (patternMatchSimple(glob, fileName)) {
                    languages.push_back(language);
                    break;
                }
            }
        }
    }

    if (!languages.empty()) {
        // Use the content type to pick among glob matches
        if (!contentType.isEmpty()) {
            for (const Language *language : languages) {
                for (const QString &mimeType : language->mimeTypes()) {
                    const QString content = contentTypeFromMimeType(mimeType);
                    if (!content.isEmpty() && contentTypeIsA(contentType, content)) {
                        // e.g., the glob matched XML, but the content type
                        // is an XML dialect
                        if (contentTypeFromMimeType(contentType) != content) {
                            if (const Language *mimeLanguage = languageForMimeType(contentType))
                                return mimeLanguage;
                        }
                        return language;
                    }
                }
            }
        }
        return languages.front();
    }
    if (!contentType.isEmpty())
        return languageForMimeType(contentType);
    return nullptr;
}

const Language *LanguageManager::guessLanguageForFile(const QString &path) const
{
    if (path.isEmpty())
        return nullptr;
    const QFileInfo info(path);
    // The original can't query the content type of missing files
    if (!info.exists())
        return nullptr;
    return guessLanguage(info.fileName(), mimeDatabase().mimeTypeForFile(info).name());
}

const Language *LanguageManager::guessLanguageForMimeType(const QString &mimeType) const
{
    return guessLanguage(QString(), contentTypeFromMimeType(mimeType));
}

std::shared_ptr<const ContextData> LanguageManager::contextData(const QString &id, QString *error) const
{
    if (const auto it = contextData_.constFind(id); it != contextData_.constEnd()) {
        if (error)
            *error = errors_.value(id);
        return *it;
    }

    std::shared_ptr<ContextData> data;
    QString message;
    if (const Language *language = this->language(id)) {
        data = std::make_shared<ContextData>(id);
        LanguageParser parser(*this, *data);
        if (!parser.parse(*language)) {
            message = parser.error();
            qCWarning(lcLanguage) << "Failed to load" << language->fileName() << ":" << message;
            data.reset();
        }
    } else {
        message = QStringLiteral("unknown language '%1'").arg(id);
    }
    contextData_.insert(id, data);
    errors_.insert(id, message);
    if (error)
        *error = message;
    return data;
}

bool patternMatchSimple(QStringView pattern, QStringView string)
{
    // Iterative wildcard matching with backtracking to the last '*'
    qsizetype p = 0;
    qsizetype s = 0;
    qsizetype starP = -1;
    qsizetype starS = 0;
    while (s < string.size()) {
        if (p < pattern.size() && pattern.at(p) == QLatin1Char('*')) {
            starP = p++;
            starS = s;
        } else if (p < pattern.size() && (pattern.at(p) == QLatin1Char('?') || pattern.at(p) == string.at(s))) {
            ++p;
            // '?' matches a whole character
            if (pattern.at(p - 1) == QLatin1Char('?') && string.at(s).isHighSurrogate() && s + 1 < string.size())
                ++s;
            ++s;
        } else if (starP >= 0) {
            p = starP + 1;
            s = ++starS;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern.at(p) == QLatin1Char('*'))
        ++p;
    return p == pattern.size();
}

} // namespace meld::syntax
