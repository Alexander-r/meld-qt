#include "linesource.h"

#include "myers.h"

namespace meld {

QStringList LineSource::lines(int lo, int hi) const
{
    auto [l, h] = pySliceBounds(lineCount(), lo, hi);
    QStringList result;
    result.reserve(h - l);
    for (int i = l; i < h; ++i)
        result.append(line(i));
    return result;
}

} // namespace meld
