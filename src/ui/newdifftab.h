#pragma once

#include "melddoc.h"

#include <QHash>

class QCheckBox;
class QPushButton;
class QStackedWidget;
class QToolButton;

namespace meld {

class NewDiffTab : public MeldDoc {
    Q_OBJECT

public:
    enum class DiffType {
        Unselected = -1,
        File = 0,
        Folder = 1,
        Version = 2,
    };

    explicit NewDiffTab(QWidget *parent = nullptr);

    void requestClose() override;
    QWidget *focusPane() const override;

Q_SIGNALS:
    //! The user asked for a comparison; empty paths are blank panes
    void diffRequested(meld::NewDiffTab::DiffType type, const QStringList &paths);
    //! The user asked to compare a file or folder with a patch applied to it
    void patchDiffRequested(meld::NewDiffTab::DiffType type, const QString &path, const QString &patchFile);

protected:
    void changeEvent(QEvent *event) override;

private:
    //! The "suggested-action" colours of the Compare button, from the
    //! current palette
    void updateSuggestedStyle();
    void onTypeToggled(QToolButton *button, bool checked);
    void chooseFile(QPushButton *button, bool folder);
    int numPaths() const;
    //! Whether the current page compares with a patch
    bool comparesWithPatch() const;
    void compare();
    void newBlank();

    DiffType diffType_ = DiffType::Unselected;
    QList<QToolButton *> typeButtons_;
    QStackedWidget *choosers_;
    QList<QPushButton *> fileChoosers_;
    QList<QPushButton *> dirChoosers_;
    QPushButton *vcChooser_;
    QCheckBox *fileThreeWay_;
    QCheckBox *dirThreeWay_;
    QCheckBox *filePatch_;
    QCheckBox *dirPatch_;
    QPushButton *blankButton_;
    QPushButton *compareButton_ = nullptr;
    QHash<QPushButton *, QString> values_;
    QString lastFolder_;
};

} // namespace meld
