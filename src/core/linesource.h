#pragma once

#include <QString>
#include <QStringList>

#include <climits>
#include <vector>

namespace meld {

/*!
 * A sequence of text lines. This replaces the duck-typed Python lists
 * and BufferLines objects that are passed around as "texts" in Meld.
 */
class LineSource {
public:
    virtual ~LineSource() = default;

    virtual int lineCount() const = 0;
    //! Line contents without line terminator; empty for out of range lines
    virtual QString line(int index) const = 0;
    //! Python slice semantics (clamped, empty for inverted ranges)
    virtual QStringList lines(int lo, int hi = INT_MAX) const;
};

//! A LineSource backed by a list of strings
class StringListSource final : public LineSource {
public:
    StringListSource() = default;
    explicit StringListSource(QStringList lines)
        : lines_(std::move(lines))
    {
    }

    int lineCount() const override { return int(lines_.size()); }
    QString line(int index) const override
    {
        return index >= 0 && index < lines_.size() ? lines_.at(index) : QString();
    }

    QStringList &list() { return lines_; }
    const QStringList &list() const { return lines_; }

private:
    QStringList lines_;
};

using Texts = std::vector<const LineSource *>;

} // namespace meld
