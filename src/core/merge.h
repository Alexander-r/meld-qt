#pragma once

#include "diffutil.h"

#include <memory>

namespace meld {

class AutoMergeDiffer : public Differ {
    Q_OBJECT

public:
    explicit AutoMergeDiffer(QObject *parent = nullptr);

    bool autoMergeEnabled = false;
    std::vector<int> unresolved;

    void replaceInSequence(int sequence, int startidx, int linesRemoved, int linesAdded,
                           const Texts &texts) override;
    int unresolvedCount() const { return int(unresolved.size()); }

protected:
    std::vector<MergedChunk> autoMerge(Using &using_, const Texts &texts) override;
};

class Merger {
public:
    Merger();
    ~Merger();

    //! The differ used for merging; owned unless replaced via setDiffer()
    Differ *differ() const { return differ_; }
    void setDiffer(Differ *differ);

    Texts texts;
    std::vector<int> unresolved;

    std::generator<bool> initialize(Texts sequences, Texts texts);
    QString merge3Files(bool markConflicts = true);
    QString merge2Files(int fromindex, int toindex);

private:
    int applyChange(const LineSource *text, const DiffChunk &change, QStringList &mergedtext) const;

    std::unique_ptr<AutoMergeDiffer> ownedDiffer_;
    Differ *differ_ = nullptr;
};

} // namespace meld
