// Qt port of GtkSourceView's context engine
//
// GtkSourceView analyses whole buffers into a tree of segments; this port
// analyses one line at a time from the state at the end of the previous
// line, which is what a QSyntaxHighlighter-style highlighter needs, and
// produces the same styles.

#include "core/syntax/contextengine.h"

#include <QElapsedTimer>
#include <QLoggingCategory>

#include <algorithm>
#include <climits>

namespace meld::syntax {

Q_LOGGING_CATEGORY(lcEngine, "meld.syntax.engine")

namespace {

//! Matches "\%{...@start}" references to the start match
const QRegularExpression &startRefRegex()
{
    static const QRegularExpression re(QStringLiteral(R"((?<!\\)(\\\\)*\\%\{(.*?)@start\})"));
    return re;
}

//! Whether a pattern uses \C, which means "single byte" in PCRE
bool findSingleByteEscape(const QString &pattern)
{
    qsizetype index = 0;
    while ((index = pattern.indexOf(QLatin1String("\\C"), index)) >= 0) {
        if (index == 0)
            return true;
        bool found = true;
        for (qsizetype slash = index - 1; slash >= 0 && pattern.at(slash) == QLatin1Char('\\'); --slash)
            found = !found;
        if (found)
            return true;
        index += 2;
    }
    return false;
}

//! g_regex_escape_string()
QString escapeRegex(const QString &text)
{
    QString escaped;
    escaped.reserve(text.size());
    for (const QChar c : text) {
        switch (c.unicode()) {
        case 0:
            escaped += QLatin1String("\\0");
            break;
        case '\\':
        case '|':
        case '(':
        case ')':
        case '[':
        case ']':
        case '{':
        case '}':
        case '^':
        case '$':
        case '*':
        case '+':
        case '?':
        case '.':
            escaped += QLatin1Char('\\');
            escaped += c;
            break;
        default:
            escaped += c;
            break;
        }
    }
    return escaped;
}

int toNumber(const QString &text)
{
    bool ok = false;
    const int number = text.toInt(&ok);
    return ok && number >= 0 ? number : -1;
}

//! The suffix of renamed duplicate group names
constexpr QLatin1StringView DuplicateSuffix("__dup");

/*!
 * Qt warns about patterns using (?J), which allows duplicate group names,
 * and doesn't look up such names. The parser adds it to the inline options
 * of regexes with the "dupnames" option, as GtkSourceView does; here it's
 * removed, and duplicate names renamed to "name__dup1" etc., which
 * groupForName() finds by their original names.
 */
QString renameDuplicateGroups(const QString &pattern)
{
    static const QRegularExpression dupNamesOption(QStringLiteral(R"((?<!\\)\(\?(i?x?)J((?:-[ix]{1,2})?)\))"));
    if (!pattern.contains(dupNamesOption))
        return pattern;
    QString renamed = pattern;
    renamed.replace(dupNamesOption, QStringLiteral("(?\\1\\2)"));

    // Named groups: (?P<name>, (?<name> and (?'name'
    static const QRegularExpression groupName(
        QStringLiteral(R"((?<!\\)((?:\\\\)*)\(\?(?:P<|<|')([A-Za-z_][A-Za-z0-9_]*)(?=[>']))"));
    QHash<QString, int> seen;
    QString result;
    qsizetype last = 0;
    for (auto it = groupName.globalMatch(renamed); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        const int count = seen[m.captured(2)]++;
        if (count == 0)
            continue;
        result += QStringView(renamed).mid(last, m.capturedEnd(2) - last);
        result += DuplicateSuffix + QString::number(count);
        last = m.capturedEnd(2);
    }
    result += QStringView(renamed).mid(last);
    return result;
}

//! A group name, without the suffix of renamed duplicates
QStringView originalGroupName(QStringView name)
{
    const qsizetype suffix = name.lastIndexOf(DuplicateSuffix);
    if (suffix > 0 && suffix + DuplicateSuffix.size() < name.size()) {
        const QStringView number = name.sliced(suffix + DuplicateSuffix.size());
        if (std::all_of(number.begin(), number.end(), [](QChar c) { return c.isDigit(); }))
            return name.first(suffix);
    }
    return name;
}

//! Compiled regexes are shared, as many contexts have the same reg_all
std::shared_ptr<SourceRegex> cachedRegex(const QString &pattern)
{
    static QHash<QString, std::shared_ptr<SourceRegex>> cache;
    if (const auto it = cache.constFind(pattern); it != cache.constEnd())
        return *it;
    QString error;
    std::shared_ptr<SourceRegex> regex = SourceRegex::create(pattern, false, &error);
    if (!regex)
        qCWarning(lcEngine) << "Cannot create a regex for all the transitions:" << error;
    cache.insert(pattern, regex);
    return regex;
}

} // namespace

// ---------------------------------------------------------------------------
// SourceRegex

std::shared_ptr<SourceRegex> SourceRegex::create(const QString &pattern, bool anchored, QString *error)
{
    if (findSingleByteEscape(pattern)) {
        if (error)
            *error = QStringLiteral("using \\C is not supported in language definitions");
        return nullptr;
    }

    auto regex = std::make_shared<SourceRegex>();
    regex->pattern_ = pattern;
    regex->anchored_ = anchored;
    if (startRefRegex().match(pattern).hasMatch()) {
        regex->resolved_ = false;
        return regex;
    }

    regex->regex_ = QRegularExpression(renameDuplicateGroups(pattern), QRegularExpression::UseUnicodePropertiesOption);
    if (!regex->regex_.isValid()) {
        if (error)
            *error = QStringLiteral("%1 in “%2”").arg(regex->regex_.errorString(), pattern);
        return nullptr;
    }
    regex->regex_.optimize();
    return regex;
}

QRegularExpressionMatch SourceRegex::match(QStringView text, qsizetype offset) const
{
    return regex_.matchView(text, offset, QRegularExpression::NormalMatch,
                            anchored_ ? QRegularExpression::AnchorAtOffsetMatchOption
                                      : QRegularExpression::NoMatchOption);
}

int SourceRegex::groupForName(const QRegularExpressionMatch &match, const QString &name) const
{
    // The group that matched, as in GtkSourceView 4. GtkSourceView 5 finds no
    // group for a duplicate name, so here-docs whose patterns use them don't
    // end at their markers (see "Differences from the original" in the README).
    const QStringList names = regex_.namedCaptureGroups();
    int first = -1;
    for (int i = 1; i < names.size(); ++i) {
        if (originalGroupName(names.at(i)) != name)
            continue;
        if (match.capturedStart(i) >= 0)
            return i;
        if (first < 0)
            first = i;
    }
    return first;
}

std::shared_ptr<SourceRegex> SourceRegex::resolve(const SourceRegex &start,
                                                  const QRegularExpressionMatch &startMatch) const
{
    if (resolved_)
        return std::make_shared<SourceRegex>(*this);

    QString expanded;
    qsizetype last = 0;
    for (auto it = startRefRegex().globalMatch(pattern_); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        expanded += QStringView(pattern_).mid(last, m.capturedStart() - last);
        const QString reference = m.captured(2);
        const int number = toNumber(reference);
        const int group = number >= 0 ? number : start.groupForName(startMatch, reference);
        QString substitution;
        if (group >= 0 && group <= startMatch.lastCapturedIndex())
            substitution = startMatch.captured(group);
        else
            qCWarning(lcEngine) << "Invalid group:" << reference;
        expanded += m.captured(1) + escapeRegex(substitution);
        last = m.capturedEnd();
    }
    expanded += QStringView(pattern_).mid(last);

    std::shared_ptr<SourceRegex> regex = create(expanded, anchored_);
    if (!regex || !regex->resolved_) {
        qCWarning(lcEngine) << "Regular expression" << pattern_ << "cannot be expanded.";
        // A regex that never matches
        regex = create(QStringLiteral("$never-match^"), anchored_);
    }
    return regex;
}

// ---------------------------------------------------------------------------
// Definitions

const std::vector<const DefinitionChild *> &ContextDefinition::expandedChildren() const
{
    if (expanded_)
        return *expanded_;

    std::vector<const DefinitionChild *> result;
    // definition_iter_next(): references to all children are replaced by
    // those children, recursively
    const auto expand = [&result](const auto &self, const ContextDefinition *definition, int depth) -> void {
        if (depth > 64) {
            qCWarning(lcEngine) << "Too deeply nested context references in" << definition->id;
            return;
        }
        for (const DefinitionChild &child : definition->children) {
            if (!child.definition)
                continue;
            if (child.isRefAll)
                self(self, child.definition, depth + 1);
            else
                result.push_back(&child);
        }
    };
    expand(expand, this, 0);
    expanded_ = std::move(result);
    return *expanded_;
}

ContextData::ContextData(const QString &languageId)
    : languageId_(languageId)
{
}

ContextData::~ContextData() = default;

int ContextData::styleIndex(const QString &styleId) const
{
    if (styleId.isEmpty())
        return -1;
    if (const auto it = styleIndices_.constFind(styleId); it != styleIndices_.constEnd())
        return *it;
    const int index = int(styleNames_.size());
    styleNames_.append(styleId);
    styleIndices_.insert(styleId, index);
    return index;
}

const Context *ContextData::rootContext() const
{
    if (!root_ && main_) {
        // The root context has no style and no end, even if the main
        // definition does
        root_.reset(new Context);
        root_->definition_ = main_;
        root_->allAncestorsExtend_ = true;
    }
    return root_.get();
}

// ---------------------------------------------------------------------------
// Context

bool Context::ancestorCanEnd() const
{
    // Root context can't be terminated; its child may not be terminated by
    // it; grandchildren look at the flag.
    return parent_ && parent_->parent_
        && (!(definition_->flags & ExtendParent) || !allAncestorsExtend_);
}

bool Context::extendsParent() const
{
    return !parent_ || !parent_->parent_ || (definition_->flags & ExtendParent);
}

bool Context::endsParent() const
{
    return parent_ && parent_->parent_ && (definition_->flags & EndParent);
}

bool Context::endsAtLineEnd() const
{
    return parent_ && (definition_->flags & EndAtLineEnd);
}

const SourceRegex *Context::regexAll() const
{
    if (regexAll_)
        return regexAll_->get();

    // create_reg_all(): the end of this context, the ends of ancestors that
    // can end it and the starts of all child contexts
    QString all = QStringLiteral("(");
    if (definition_->type == ContextDefinition::Type::Container && definition_->end) {
        const SourceRegex *end = definition_->end->isResolved() ? definition_->end.get() : end_.get();
        if (end) {
            all += end->pattern();
            all += QLatin1Char('|');
        }
    }
    for (const Context *tmp = this; tmp->ancestorCanEnd(); tmp = tmp->parent_) {
        if (tmp->extendsParent())
            continue;
        if (tmp->parent_->end_) {
            all += tmp->parent_->end_->pattern();
            all += QLatin1Char('|');
        } else if (tmp->parent_->endsAtLineEnd()) {
            all += QStringLiteral("$|");
        }
    }
    for (const DefinitionChild *child : definition_->expandedChildren()) {
        const ContextDefinition *definition = child->definition;
        const SourceRegex *regex = definition->type == ContextDefinition::Type::Container ? definition->start.get()
                                                                                          : definition->match.get();
        if (regex) {
            all += regex->pattern();
            all += QLatin1Char('|');
        }
    }
    if (all.size() > 1)
        all.chop(1);
    all += QLatin1Char(')');

    regexAll_ = cachedRegex(all);
    return regexAll_->get();
}

const Context *Context::child(const DefinitionChild &child, const SourceRegex *start,
                              const QRegularExpressionMatch &startMatch, const ContextData &data) const
{
    // create_child_context(): contexts are shared for each definition in a
    // parent, or for each start text if the end refers to the start match.
    const ContextDefinition *definition = child.definition;
    Children &children = children_[definition];
    const bool fixed = definition->type != ContextDefinition::Type::Container || !definition->end
        || definition->end->isResolved();
    std::unique_ptr<Context> *slot = &children.fixed;
    if (!fixed)
        slot = &children.byStartText[startMatch.captured(0)];
    if (*slot)
        return slot->get();

    auto context = std::unique_ptr<Context>(new Context);
    context->definition_ = definition;
    context->parent_ = this;
    QString style = child.overrideStyle ? child.style : definition->defaultStyle;
    context->ignoreChildrenStyle_ = child.overrideStyle ? child.overrideStyleDeep : false;
    if (ignoreChildrenStyle_) {
        context->ignoreChildrenStyle_ = true;
        style.clear();
    }
    context->style_ = data.styleIndex(style);
    context->allAncestorsExtend_ = allAncestorsExtend_ && extendsParent();
    if (definition->type == ContextDefinition::Type::Container && definition->end) {
        if (definition->end->isResolved())
            context->end_ = definition->end;
        else if (start)
            context->end_ = definition->end->resolve(*start, startMatch);
    }
    *slot = std::move(context);
    return slot->get();
}

// ---------------------------------------------------------------------------
// Line analysis

namespace {

struct WorkFrame {
    const Context *context = nullptr;
    QList<const ContextDefinition *> onceOnly;
    //! Column where this context's segment started, or -1 if it started
    //! on an earlier line
    int startCol = -1;
    //! Column from which the context's style applies on this line
    int styleFrom = 0;
    //! The last child segment on this line, for the zero-length check
    const Context *lastChild = nullptr;
    int lastChildStart = INT_MIN;
    int lastChildEnd = INT_MIN + 1;
};

class LineAnalysis {
public:
    LineAnalysis(const ContextData &data, QStringView text, bool firstLine, std::vector<StyleSpan> &spans)
        : data_(data)
        , text_(text)
        , length_(int(text.size()))
        , firstLine_(firstLine)
        , spans_(spans)
    {
    }

    bool run(const LineState &state)
    {
        for (const LineState::Frame &frame : state.frames)
            stack_.push_back(WorkFrame{frame.context, frame.onceOnly});
        if (firstLine_ && !stack_.empty())
            stack_.front().startCol = 0;

        QElapsedTimer timer;
        timer.start();
        int pos = 0;
        while (pos <= length_) {
            if (!nextSegment(pos))
                break;
            if (timer.elapsed() > LineHighlighter::MaxTimeForOneLine)
                return false;
        }

        // Verify if we need to close the context because we are at the end
        // of the line.
        const Context *top = stack_.back().context;
        if (top->ancestorCanEnd() || top->endsAtLineEnd())
            checkLineEnd();

        for (int depth = int(stack_.size()) - 1; depth >= 0; --depth)
            emitContext(stack_[size_t(depth)], depth, length_, 0);
        return true;
    }

    LineStatePtr state() const
    {
        auto state = std::make_shared<LineState>();
        for (const WorkFrame &frame : stack_)
            state->frames.push_back({frame.context, frame.onceOnly});
        return state;
    }

private:
    int nextChar(int pos) const
    {
        if (pos < length_ && text_.at(pos).isHighSurrogate() && pos + 1 < length_ && text_.at(pos + 1).isLowSurrogate())
            return pos + 2;
        return pos + 1;
    }

    int depth() const { return int(stack_.size()) - 1; }

    void addSpan(int start, int end, int style, int priority)
    {
        if (style >= 0 && start < end)
            spans_.push_back({start, end, style, priority});
    }

    //! The style of a context's segment that ends at `end` on this line,
    //! with an end match of `endLength`
    void emitContext(const WorkFrame &frame, int depth, int end, int endLength)
    {
        int styleEnd = end;
        if (frame.context->definition()->flags & StyleInside)
            styleEnd -= endLength;
        // GtkSourceView complains and skips the style in this case
        if (frame.styleFrom <= styleEnd)
            addSpan(frame.styleFrom, styleEnd, frame.context->style(), 2 * depth);
    }

    void emitSubPatterns(const Context *context, const QRegularExpressionMatch &match, const SourceRegex &regex,
                         SubPatternWhere where, int depth)
    {
        const size_t firstSpan = spans_.size();
        for (const SubPatternDefinition &sp : context->definition()->subPatterns) {
            if (sp.where != where || sp.style.isEmpty())
                continue;
            const int group = sp.number >= 0 ? sp.number : regex.groupForName(match, sp.name);
            if (group < 0 || group > match.lastCapturedIndex())
                continue;
            const int start = int(match.capturedStart(group));
            const int end = int(match.capturedEnd(group));
            const int style = data_.styleIndex(sp.style);
            if (style < 0 || start < 0 || start == end)
                continue;
            // The first of several for the same text overrides the others.
            // In GtkSourceView, this depends on when their tags were created
            // in the document, but tags created for this match have this
            // order, as those of later sub-patterns are created first.
            const auto sameText = std::find_if(spans_.begin() + qsizetype(firstSpan), spans_.end(),
                                               [start, end](const StyleSpan &span) {
                                                   return span.start == start && span.end == end;
                                               });
            spans_.insert(sameText, {start, end, style, 2 * depth + 1});
        }
    }

    //! Close the current context's segment at `end`
    void popFrame(int end, int endLength)
    {
        WorkFrame frame = std::move(stack_.back());
        stack_.pop_back();
        emitContext(frame, int(stack_.size()), end, endLength);
        WorkFrame &parent = stack_.back();
        parent.lastChild = frame.context;
        parent.lastChildStart = frame.startCol;
        parent.lastChildEnd = end;
    }

    //! ancestor_context_ends_here(): the outermost ancestor that can end
    //! `context` and whose end matches at `pos`
    const Context *ancestorContextEndsHere(const Context *context, int pos) const
    {
        std::vector<const Context *> checkAncestors;
        for (const Context *current = context; current->ancestorCanEnd(); current = current->parent()) {
            if (!current->extendsParent())
                checkAncestors.push_back(current->parent());
        }
        for (auto it = checkAncestors.rbegin(); it != checkAncestors.rend(); ++it) {
            const SourceRegex *end = (*it)->end();
            if (end && end->isResolved() && end->match(text_, pos).hasMatch())
                return *it;
        }
        return nullptr;
    }

    /*!
     * can_apply_match(): whether a match of `regex` starting at `start`
     * can be used by `context`; an ancestor that ends inside the match cuts
     * it short. Updates `match` and sets `matchEnd`.
     */
    bool canApplyMatch(const Context *context, int start, const SourceRegex &regex, QRegularExpressionMatch &match,
                       int &matchEnd) const
    {
        const int endMatchPos = int(match.capturedEnd(0));
        bool ancestorEnds = false;
        int pos = endMatchPos;
        // there is no middle of zero-length match
        if (context->ancestorCanEnd() && start < endMatchPos) {
            for (pos = nextChar(start); pos < endMatchPos; pos = nextChar(pos)) {
                if (ancestorContextEndsHere(context, pos)) {
                    ancestorEnds = true;
                    break;
                }
            }
            if (!ancestorEnds)
                pos = endMatchPos;
        }

        if (ancestorEnds) {
            // An ancestor ends in the middle of the match, so we verify if
            // the regex matches the text before the end of the ancestor.
            // For instance in C a net-address context matches even if it
            // contains the end of a multi-line comment.
            QRegularExpressionMatch truncated = regex.match(text_.first(pos), start);
            if (!truncated.hasMatch())
                return false;
            match = truncated;
        }
        matchEnd = pos;
        return true;
    }

    bool containerStartsHere(const DefinitionChild &child, int &pos)
    {
        const ContextDefinition *definition = child.definition;
        // We can have a container context definition (i.e. the main
        // language definition) without start.
        if (!definition->start)
            return false;
        QRegularExpressionMatch match = definition->start->match(text_, pos);
        if (!match.hasMatch())
            return false;

        WorkFrame &state = stack_.back();
        const Context *context = state.context->child(child, definition->start.get(), match, data_);
        int matchEnd = 0;
        if (!canApplyMatch(context, pos, *definition->start, match, matchEnd))
            return false;

        // A context that starts and ends at the same position would give an
        // infinite loop, so a zero-length context can't start right after
        // another zero-length segment of the same context.
        if (pos == matchEnd && state.lastChild == context && state.lastChildStart == state.lastChildEnd
            && state.lastChildStart == pos) {
            return false;
        }

        if (definition->flags & OnceOnly)
            state.onceOnly.append(definition);
        state.lastChild = context;
        state.lastChildStart = pos;
        state.lastChildEnd = INT_MIN;

        WorkFrame frame;
        frame.context = context;
        frame.startCol = pos;
        frame.styleFrom = pos;
        if (definition->flags & StyleInside) {
            const int startEnd = int(match.capturedEnd(0));
            frame.styleFrom = (int(match.capturedStart(0)) == pos && startEnd <= matchEnd) ? startEnd : pos;
        }
        stack_.push_back(std::move(frame));
        emitSubPatterns(context, match, *definition->start, SubPatternWhere::Start, depth());
        pos = matchEnd;
        return true;
    }

    bool simpleStartsHere(const DefinitionChild &child, int &pos)
    {
        const ContextDefinition *definition = child.definition;
        if (!definition->match)
            return false;
        QRegularExpressionMatch match = definition->match->match(text_, pos);
        if (!match.hasMatch())
            return false;

        WorkFrame &state = stack_.back();
        const Context *context = state.context->child(child, nullptr, match, data_);
        int matchEnd = 0;
        if (!canApplyMatch(context, pos, *definition->match, match, matchEnd))
            return false;

        // A zero-length match returns to the same state, unless it ends the
        // parent; even then, it mustn't be at the start of the parent.
        if (pos == matchEnd && (!context->endsParent() || state.startCol == pos))
            return false;

        if (pos != matchEnd) {
            const int childDepth = depth() + 1;
            addSpan(pos, matchEnd, context->style(), 2 * childDepth);
            emitSubPatterns(context, match, *definition->match, SubPatternWhere::Default, childDepth);
            if (definition->flags & OnceOnly)
                state.onceOnly.append(definition);
            state.lastChild = context;
            state.lastChildStart = pos;
            state.lastChildEnd = matchEnd;
        }

        // Terminate parent if needed
        if (context->endsParent()) {
            do {
                popFrame(matchEnd, 0);
            } while (stack_.back().context->endsParent());
        }

        pos = matchEnd;
        return true;
    }

    bool childStartsHere(const DefinitionChild &child, int &pos)
    {
        if (child.definition->type == ContextDefinition::Type::Simple)
            return simpleStartsHere(child, pos);
        return containerStartsHere(child, pos);
    }

    //! next_segment(): find what happens next in the line, from `linePos`
    bool nextSegment(int &linePos)
    {
        int pos = linePos;
        while (pos <= length_) {
            WorkFrame &state = stack_.back();
            const Context *context = state.context;

            if (const SourceRegex *all = context->regexAll()) {
                const QRegularExpressionMatch match = all->match(text_, pos);
                if (!match.hasMatch())
                    return false;
                pos = int(match.capturedStart(0));
            }

            // Does an ancestor end here?
            if (context->ancestorCanEnd()) {
                if (const Context *terminating = ancestorContextEndsHere(context, pos)) {
                    // Close all the descendants; the terminating context is
                    // closed by the next call.
                    while (stack_.back().context != terminating)
                        popFrame(pos, 0);
                    linePos = pos;
                    return true;
                }
            }

            // Does the current context end here?
            QRegularExpressionMatch endMatch;
            const SourceRegex *end = context->definition()->end ? context->end() : nullptr;
            if (end && end->isResolved())
                endMatch = end->match(text_, pos);
            const bool contextEndFound = endMatch.hasMatch();

            for (const DefinitionChild *child : context->definition()->expandedChildren()) {
                const ContextDefinition *definition = child->definition;
                // If the child does not extend the parent and the current
                // context could end here, it's not examined.
                if (!(definition->flags & ExtendParent) && contextEndFound)
                    continue;
                if ((definition->flags & FirstLineOnly) && !firstLine_)
                    continue;
                if ((definition->flags & OnceOnly) && stack_.back().onceOnly.contains(definition))
                    continue;
                if (childStartsHere(*child, pos)) {
                    linePos = pos;
                    return true;
                }
            }

            if (contextEndFound) {
                // The parent context may still end in the middle of the end
                // match.
                int matchEnd = 0;
                if (canApplyMatch(context, pos, *end, endMatch, matchEnd)) {
                    const int endLength = int(endMatch.capturedEnd(0)) == matchEnd
                        ? matchEnd - int(endMatch.capturedStart(0))
                        : 0;
                    emitSubPatterns(context, endMatch, *end, SubPatternWhere::End, depth());
                    const Context *ended = context;
                    popFrame(matchEnd, endLength);
                    while (ended->endsParent()) {
                        ended = stack_.back().context;
                        popFrame(matchEnd, 0);
                    }
                    linePos = matchEnd;
                    return true;
                }
            }

            // Nothing new at this position, go to next char.
            pos = nextChar(pos);
        }
        return false;
    }

    //! check_line_end(): close the contexts that end at line ends
    void checkLineEnd()
    {
        int terminating = -1;
        for (int i = depth(); i >= 0; --i) {
            const Context *context = stack_[size_t(i)].context;
            if (context->endsAtLineEnd())
                terminating = i;
            else if (!context->ancestorCanEnd())
                break;
        }
        if (terminating > 0) {
            while (depth() >= terminating)
                popFrame(length_, 0);
        }
    }

    const ContextData &data_;
    QStringView text_;
    int length_;
    bool firstLine_;
    std::vector<StyleSpan> &spans_;
    std::vector<WorkFrame> stack_;
};

//! PCRE can't match text with unpaired surrogates
QString sanitizedText(QStringView text, bool *changed)
{
    *changed = false;
    QString result;
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        bool valid = true;
        if (c.isHighSurrogate())
            valid = i + 1 < text.size() && text.at(i + 1).isLowSurrogate();
        else if (c.isLowSurrogate())
            valid = i > 0 && text.at(i - 1).isHighSurrogate();
        if (!valid && !*changed) {
            result = text.toString();
            *changed = true;
        }
        if (!valid)
            result[i] = QChar::ReplacementCharacter;
    }
    return result;
}

} // namespace

LineHighlighter::LineHighlighter(std::shared_ptr<const ContextData> data)
    : data_(std::move(data))
{
    auto state = std::make_shared<LineState>();
    if (const Context *root = data_->rootContext())
        state->frames.push_back({root, {}});
    initial_ = std::move(state);
}

LineStatePtr LineHighlighter::highlightLine(QStringView text, bool firstLine, const LineStatePtr &state,
                                            std::vector<StyleSpan> &spans) const
{
    spans.clear();
    if (!state || state->frames.empty())
        return state;

    bool changed = false;
    const QString sanitized = sanitizedText(text, &changed);
    LineAnalysis analysis(*data_, changed ? QStringView(sanitized) : text, firstLine, spans);
    if (!analysis.run(*state)) {
        qCWarning(lcEngine) << "Highlighting a single line took too much time, syntax highlighting will be disabled";
        spans.clear();
        return nullptr;
    }
    // Lines often end in the state they started in; sharing it saves
    // memory for documents' per-line states
    LineStatePtr next = analysis.state();
    return *next == *state ? state : next;
}

} // namespace meld::syntax
