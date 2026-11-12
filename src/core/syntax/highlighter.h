#pragma once

#include "core/syntax/contextengine.h"
#include "core/syntax/stylescheme.h"

#include <QColor>
#include <QFont>
#include <QHash>
#include <QList>
#include <QObject>
#include <QTextCharFormat>
#include <QTextLayout>
#include <QTimer>

#include <climits>
#include <memory>
#include <optional>

class QTextBlock;
class QTextDocument;

namespace meld::syntax {

class Language;

/*!
 * Highlights the syntax of a document with a language definition and a
 * style scheme
 *
 * Unlike QSyntaxHighlighter, this is lazy: blocks are highlighted in order
 * when they're about to be shown (ensureHighlighted()), and the rest in idle
 * time, so that large documents open quickly. After edits, blocks whose
 * starting state didn't change keep their highlighting. Formats are set as
 * the blocks' layout formats, so they don't change the document's contents
 * or its undo history.
 */
class SyntaxHighlighter : public QObject {
    Q_OBJECT

public:
    explicit SyntaxHighlighter(QTextDocument *document);
    ~SyntaxHighlighter() override;

    QTextDocument *document() const { return document_; }

    //! The language to highlight, or nullptr for plain text
    const Language *language() const { return language_; }
    void setLanguage(const Language *language);
    //! Whether to highlight syntax (GtkSourceBuffer:highlight-syntax)
    bool highlightSyntax() const { return enabled_; }
    void setHighlightSyntax(bool enabled);
    const StyleScheme *styleScheme() const { return scheme_; }
    void setStyleScheme(const StyleScheme *scheme);
    //! The font that styles' scales (e.g., for headings) are relative to
    void setBaseFont(const QFont &font);

    //! Whether highlighting is enabled, and the language could be loaded
    bool isActive() const;

    /*!
     * Highlight all blocks up to and including `blockNumber`
     *
     * With a time limit (in milliseconds), this may stop early; the rest is
     * highlighted in idle time, as GtkSourceView does for far away parts of
     * large documents.
     */
    void ensureHighlighted(int blockNumber, int timeLimit = -1);
    void highlightAll() { ensureHighlighted(INT_MAX); }
    //! The number of leading blocks that are highlighted
    int highlightedBlocks() const { return validBlocks_; }

    //! The background of the whole line, for styles that have one
    std::optional<QColor> lineBackground(const QTextBlock &block) const;

    //! The style for a style id, following the language's style mappings
    //! for styles the scheme doesn't have
    std::optional<Style> resolveStyle(const QString &styleId) const;

    //! The text format for a style
    static QTextCharFormat format(const Style &style, const QFont &baseFont);

Q_SIGNALS:
    void languageChanged(const meld::syntax::Language *language);
    //! Highlighting was disabled, because a line took too long
    void highlightingDisabled();

private:
    struct BlockData;

    void onContentsChange(int position, int charsRemoved, int charsAdded);
    //! Highlight blocks up to `lastBlock`, until `deadline` (in ms) if set
    void highlightBlocks(int lastBlock, int deadline = -1);
    void onIdle();
    void restart();
    void clearFormats();
    QList<QTextLayout::FormatRange> formats(const std::vector<StyleSpan> &spans, int length,
                                            std::optional<QColor> *lineBackground);
    const QTextCharFormat &stackFormat(const QList<int> &stack, std::optional<QColor> *lineBackground);
    const std::optional<Style> &styleFor(int styleIndex);

    QTextDocument *document_;
    const Language *language_ = nullptr;
    bool enabled_ = true;
    bool timedOut_ = false;
    const StyleScheme *scheme_ = nullptr;
    QFont baseFont_;

    std::shared_ptr<const ContextData> data_;
    std::unique_ptr<LineHighlighter> highlighter_;
    //! Changes when highlighting must be redone
    unsigned generation_ = 1;
    int validBlocks_ = 0;
    //! Whether formats were set that need clearing
    bool hasFormats_ = false;

    QHash<int, std::optional<Style>> styles_;
    struct StackFormat {
        QTextCharFormat format;
        std::optional<QColor> lineBackground;
    };
    QHash<QList<int>, StackFormat> stackFormats_;
    QTimer idle_;
};

} // namespace meld::syntax
