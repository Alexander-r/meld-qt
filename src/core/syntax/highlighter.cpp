// Syntax highlighting of text documents, as GtkSourceBuffer does it

#include "core/syntax/highlighter.h"

#include "core/syntax/language.h"

#include <QElapsedTimer>
#include <QLoggingCategory>
#include <QTextBlock>
#include <QTextDocument>

#include <algorithm>

namespace meld::syntax {

Q_LOGGING_CATEGORY(lcHighlighter, "meld.syntax.highlighter")

namespace {

//! As GtkSourceView's context engine, for style map-to loops
constexpr int MaxStyleDependencyDepth = 50;
//! Idle highlighting works in slices of this many milliseconds
constexpr int IdleSlice = 20;

} // namespace

//! The highlighting state of a block
struct SyntaxHighlighter::BlockData final : QTextBlockUserData {
    //! The state the block was highlighted from, and the state after it
    LineStatePtr startState;
    LineStatePtr endState;
    unsigned generation = 0;
    //! The block's text changed since it was highlighted
    bool dirty = false;
    std::optional<QColor> lineBackground;
};

SyntaxHighlighter::SyntaxHighlighter(QTextDocument *document)
    : QObject(document)
    , document_(document)
{
    idle_.setInterval(0);
    connect(&idle_, &QTimer::timeout, this, &SyntaxHighlighter::onIdle);
    connect(document_, &QTextDocument::contentsChange, this, &SyntaxHighlighter::onContentsChange);
}

SyntaxHighlighter::~SyntaxHighlighter() = default;

void SyntaxHighlighter::setLanguage(const Language *language)
{
    if (language == language_)
        return;
    language_ = language;
    data_.reset();
    highlighter_.reset();
    timedOut_ = false;
    if (language_) {
        QString error;
        data_ = LanguageManager::instance().contextData(language_->id(), &error);
        if (data_)
            highlighter_ = std::make_unique<LineHighlighter>(data_);
        else
            qCWarning(lcHighlighter) << "Couldn't load language" << language_->id() << error;
    }
    styles_.clear();
    stackFormats_.clear();
    restart();
    Q_EMIT languageChanged(language_);
}

void SyntaxHighlighter::setHighlightSyntax(bool enabled)
{
    if (enabled == enabled_)
        return;
    enabled_ = enabled;
    restart();
}

void SyntaxHighlighter::setStyleScheme(const StyleScheme *scheme)
{
    if (scheme == scheme_)
        return;
    scheme_ = scheme;
    styles_.clear();
    stackFormats_.clear();
    restart();
}

void SyntaxHighlighter::setBaseFont(const QFont &font)
{
    if (font == baseFont_)
        return;
    baseFont_ = font;
    // Only scaled styles depend on the font
    stackFormats_.clear();
    restart();
}

bool SyntaxHighlighter::isActive() const
{
    return enabled_ && highlighter_ && !timedOut_;
}

void SyntaxHighlighter::restart()
{
    ++generation_;
    validBlocks_ = 0;
    if (!isActive()) {
        idle_.stop();
        clearFormats();
        return;
    }
    idle_.start();
}

void SyntaxHighlighter::clearFormats()
{
    if (!hasFormats_)
        return;
    hasFormats_ = false;
    for (QTextBlock block = document_->begin(); block.isValid(); block = block.next()) {
        if (auto *data = static_cast<BlockData *>(block.userData()))
            data->lineBackground.reset();
        if (!block.layout()->formats().isEmpty())
            block.layout()->clearFormats();
    }
    document_->markContentsDirty(0, document_->characterCount());
}

void SyntaxHighlighter::onContentsChange(int position, int charsRemoved, int charsAdded)
{
    Q_UNUSED(charsRemoved);
    if (!isActive())
        return;
    // Blocks containing changed text need highlighting; the rest keep theirs
    // if their starting states don't change
    const QTextBlock first = document_->findBlock(position);
    const QTextBlock last = document_->findBlock(position + charsAdded);
    for (QTextBlock block = first; block.isValid(); block = block.next()) {
        if (auto *data = static_cast<BlockData *>(block.userData()))
            data->dirty = true;
        if (block == last)
            break;
    }
    if (first.isValid())
        validBlocks_ = std::min(validBlocks_, first.blockNumber());
    else
        validBlocks_ = std::min(validBlocks_, std::max(0, document_->blockCount() - 1));
    idle_.start();
}

void SyntaxHighlighter::ensureHighlighted(int blockNumber, int timeLimit)
{
    if (!isActive() || validBlocks_ > blockNumber || validBlocks_ >= document_->blockCount())
        return;
    highlightBlocks(blockNumber, timeLimit);
    if (isActive() && validBlocks_ < document_->blockCount())
        idle_.start();
}

void SyntaxHighlighter::onIdle()
{
    if (!isActive() || validBlocks_ >= document_->blockCount()) {
        idle_.stop();
        return;
    }
    highlightBlocks(INT_MAX, IdleSlice);
    if (!isActive() || validBlocks_ >= document_->blockCount())
        idle_.stop();
}

void SyntaxHighlighter::highlightBlocks(int lastBlock, int deadline)
{
    QElapsedTimer timer;
    timer.start();

    QTextBlock block = document_->findBlockByNumber(validBlocks_);
    LineStatePtr state = highlighter_->initialState();
    if (validBlocks_ > 0) {
        const auto *previous = static_cast<const BlockData *>(block.previous().userData());
        Q_ASSERT(previous && previous->endState);
        state = previous->endState;
    }

    // The range of blocks whose formats changed, to lay out again
    int changedFrom = -1;
    int changedTo = -1;
    std::vector<StyleSpan> spans;
    while (block.isValid() && block.blockNumber() <= lastBlock) {
        auto *data = static_cast<BlockData *>(block.userData());
        if (!data) {
            data = new BlockData;
            block.setUserData(data);
        }
        const bool current = data->generation == generation_ && !data->dirty && data->startState
            && (data->startState == state || *data->startState == *state);
        if (!current) {
            const QString text = block.text();
            LineStatePtr end = highlighter_->highlightLine(text, block.blockNumber() == 0, state, spans);
            if (!end) {
                // GtkSourceView gives up on documents with lines that take
                // too long
                timedOut_ = true;
                idle_.stop();
                clearFormats();
                Q_EMIT highlightingDisabled();
                return;
            }
            data->startState = state;
            data->endState = std::move(end);
            data->generation = generation_;
            data->dirty = false;
            block.layout()->setFormats(formats(spans, int(text.size()), &data->lineBackground));
            hasFormats_ = true;
            if (changedFrom < 0)
                changedFrom = block.position();
            changedTo = block.position() + block.length();
        }
        state = data->endState;
        validBlocks_ = block.blockNumber() + 1;
        block = block.next();
        if (deadline >= 0 && timer.elapsed() >= deadline)
            break;
    }
    if (changedFrom >= 0) {
        // Lay out the blocks again with their new formats; this doesn't emit
        // QTextDocument::contentsChange()
        document_->markContentsDirty(changedFrom, std::min(changedTo, document_->characterCount()) - changedFrom);
    }
}

std::optional<QColor> SyntaxHighlighter::lineBackground(const QTextBlock &block) const
{
    if (!isActive() || block.blockNumber() >= validBlocks_)
        return std::nullopt;
    const auto *data = static_cast<const BlockData *>(block.userData());
    return data ? data->lineBackground : std::nullopt;
}

QList<QTextLayout::FormatRange> SyntaxHighlighter::formats(const std::vector<StyleSpan> &spans, int length,
                                                           std::optional<QColor> *lineBackground)
{
    lineBackground->reset();
    QList<QTextLayout::FormatRange> ranges;
    if (spans.empty() || length == 0)
        return ranges;

    // Spans nest, and inner ones take precedence; split the line where any
    // span starts or ends, and merge the styles of the spans covering each
    // part in order of priority.
    std::vector<int> bounds;
    bounds.reserve(spans.size() * 2);
    for (const StyleSpan &span : spans) {
        bounds.push_back(std::clamp(span.start, 0, length));
        bounds.push_back(std::clamp(span.end, 0, length));
    }
    std::sort(bounds.begin(), bounds.end());
    bounds.erase(std::unique(bounds.begin(), bounds.end()), bounds.end());

    std::vector<const StyleSpan *> covering;
    for (size_t i = 0; i + 1 < bounds.size(); ++i) {
        const int start = bounds[i];
        const int end = bounds[i + 1];
        covering.clear();
        for (const StyleSpan &span : spans) {
            if (span.style >= 0 && span.start <= start && span.end >= end)
                covering.push_back(&span);
        }
        if (covering.empty())
            continue;
        std::stable_sort(covering.begin(), covering.end(),
                         [](const StyleSpan *a, const StyleSpan *b) { return a->priority < b->priority; });
        QList<int> stack;
        for (const StyleSpan *span : covering) {
            if (stack.isEmpty() || stack.constLast() != span->style)
                stack.append(span->style);
        }
        std::optional<QColor> background;
        const QTextCharFormat &format = stackFormat(stack, &background);
        if (start == 0)
            *lineBackground = background;
        if (format.properties().isEmpty())
            continue;
        if (!ranges.isEmpty() && ranges.constLast().start + ranges.constLast().length == start
            && ranges.constLast().format == format) {
            ranges.last().length += end - start;
        } else {
            ranges.append({start, end - start, format});
        }
    }
    return ranges;
}

const QTextCharFormat &SyntaxHighlighter::stackFormat(const QList<int> &stack, std::optional<QColor> *lineBackground)
{
    auto it = stackFormats_.find(stack);
    if (it == stackFormats_.end()) {
        // Like text tags, each style sets only its own attributes, and
        // those of higher priority styles win
        Style merged;
        for (const int styleIndex : stack) {
            const std::optional<Style> &style = styleFor(styleIndex);
            if (!style)
                continue;
            const auto merge = [](auto &target, const auto &value) {
                if (value)
                    target = value;
            };
            merge(merged.foreground, style->foreground);
            merge(merged.background, style->background);
            merge(merged.lineBackground, style->lineBackground);
            merge(merged.bold, style->bold);
            merge(merged.italic, style->italic);
            merge(merged.strikethrough, style->strikethrough);
            merge(merged.underline, style->underline);
            merge(merged.underlineColor, style->underlineColor);
            merge(merged.scale, style->scale);
        }
        it = stackFormats_.insert(stack, {format(merged, baseFont_), merged.lineBackground});
    }
    *lineBackground = it->lineBackground;
    return it->format;
}

const std::optional<Style> &SyntaxHighlighter::styleFor(int styleIndex)
{
    auto it = styles_.find(styleIndex);
    if (it == styles_.end())
        it = styles_.insert(styleIndex, resolveStyle(data_->styleName(styleIndex)));
    return *it;
}

std::optional<Style> SyntaxHighlighter::resolveStyle(const QString &styleId) const
{
    if (!scheme_ || !data_)
        return std::nullopt;
    // As set_tag_style(): styles the scheme doesn't have fall back to the
    // styles they map to
    std::optional<Style> style = scheme_->style(styleId);
    QString mapTo = styleId;
    for (int guard = 0; !style; ++guard) {
        if (guard > MaxStyleDependencyDepth) {
            qCWarning(lcHighlighter) << "Potential style loop detected for style" << styleId;
            break;
        }
        mapTo = data_->styleFallback(mapTo);
        if (mapTo.isEmpty())
            break;
        style = scheme_->style(mapTo);
    }
    return style;
}

QTextCharFormat SyntaxHighlighter::format(const Style &style, const QFont &baseFont)
{
    QTextCharFormat format;
    if (style.foreground)
        format.setForeground(*style.foreground);
    if (style.background)
        format.setBackground(*style.background);
    if (style.bold)
        format.setFontWeight(*style.bold ? QFont::Bold : QFont::Normal);
    if (style.italic)
        format.setFontItalic(*style.italic);
    if (style.strikethrough)
        format.setFontStrikeOut(*style.strikethrough);
    if (style.underline) {
        // Pango's underlines: none, single, double, low, error, and the
        // "-line" variants of single, double and error
        switch (*style.underline) {
        case 0:
            format.setUnderlineStyle(QTextCharFormat::NoUnderline);
            break;
        case 4:
        case 7:
            format.setUnderlineStyle(QTextCharFormat::WaveUnderline);
            break;
        default:
            format.setUnderlineStyle(QTextCharFormat::SingleUnderline);
            break;
        }
    }
    if (style.underlineColor)
        format.setUnderlineColor(*style.underlineColor);
    if (style.scale && baseFont.pointSizeF() > 0)
        format.setFontPointSize(baseFont.pointSizeF() * *style.scale);
    return format;
}

} // namespace meld::syntax
