#pragma once

#include <QToolButton>

#include <optional>

class QLineEdit;

namespace meld {

//! A button showing a shortened path, with a popover for the full path
class PathLabel : public QToolButton {
    Q_OBJECT

public:
    explicit PathLabel(QWidget *parent = nullptr);

    QString file() const { return file_; }
    void setFile(const QString &file);
    std::optional<QString> parentFile() const { return parentFile_; }
    void setParentFile(const std::optional<QString> &parent);
    QString customLabel() const { return customLabel_; }
    void setCustomLabel(const QString &label);
    void setEmptyLabel(const QString &label);

    //! The label as displayed (custom, shortened path, or placeholder)
    QString displayLabel() const;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void updatePaths(const std::optional<QString> &parent, const QString &descendant);
    QString formatPath() const;
    void updateDisplay();
    void showPopup();

    QString file_;
    std::optional<QString> parentFile_;
    QString pathLabel_;
    QString customLabel_;
    QString emptyLabel_;
};

//! A button that opens a file chooser and reports the selected file
class FileButton : public QToolButton {
    Q_OBJECT

public:
    enum class Mode {
        OpenFile,
        SelectFolder,
    };

    FileButton(int pane, Mode mode, const QString &dialogLabel, QWidget *parent = nullptr);

    QString file() const { return file_; }
    void setFile(const QString &file) { file_ = file; }

Q_SIGNALS:
    void fileSelected(int pane, const QString &file);

private:
    void chooseFile();

    int pane_;
    Mode mode_;
    QString dialogLabel_;
    QString file_;
};

//! Open the folder containing `path` in the system file manager
void openContainingFolder(const QString &path);

} // namespace meld
