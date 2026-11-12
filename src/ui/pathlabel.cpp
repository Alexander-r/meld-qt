#include "pathlabel.h"

#include "core/archive.h"
#include "core/iohelpers.h"
#include "icons.h"

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLoggingCategory>
#include <QMenu>
#include <QPainter>
#include <QPushButton>
#include <QStyleOptionToolButton>
#include <QStylePainter>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidgetAction>

namespace meld {

Q_LOGGING_CATEGORY(lcPathLabel, "meld.pathlabel")

void openContainingFolder(const QString &path)
{
    if (path.isEmpty())
        return;
    const QFileInfo info(path);
    const QString folder = info.isDir() ? info.absoluteFilePath() : info.absolutePath();
    QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
}

PathLabel::PathLabel(QWidget *parent)
    : QToolButton(parent)
{
    setPopupMode(QToolButton::InstantPopup);
    setToolButtonStyle(Qt::ToolButtonTextOnly);
    setAutoRaise(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    connect(this, &QToolButton::clicked, this, &PathLabel::showPopup);
    updateDisplay();
}

void PathLabel::setFile(const QString &file)
{
    if (file == file_)
        return;
    try {
        updatePaths(parentFile_, file);
    } catch (const PathError &e) {
        qCWarning(lcPathLabel) << "Error setting file:" << e.what();
    }
}

void PathLabel::setParentFile(const std::optional<QString> &parent)
{
    if (parent == parentFile_)
        return;
    try {
        updatePaths(parent, file_);
    } catch (const PathError &e) {
        qCWarning(lcPathLabel) << "Error setting parent file:" << e.what();
    }
}

void PathLabel::setCustomLabel(const QString &label)
{
    customLabel_ = label;
    updateDisplay();
}

void PathLabel::setEmptyLabel(const QString &label)
{
    emptyLabel_ = label;
    updateDisplay();
}

QString PathLabel::displayLabel() const
{
    if (!customLabel_.isEmpty())
        return customLabel_;
    if (!pathLabel_.isEmpty())
        return pathLabel_;
    if (!emptyLabel_.isEmpty())
        return emptyLabel_;
    return tr("Unnamed file");
}

QString PathLabel::formatPath() const
{
    if (file_.isEmpty())
        return {};
    if (const QString archivePath = archive::formatArchiveRelativePath(file_); !archivePath.isEmpty())
        return archivePath;
    if (parentFile_)
        return formatParentRelativePath(*parentFile_, file_);
    // If we have no parent yet but have a descendant, we'll use the
    // descendant name as the better-than-nothing label.
    return formatHomeRelativePath(file_);
}

void PathLabel::updatePaths(const std::optional<QString> &parent, const QString &descendant)
{
    // If either of the parent or the main files are not set, the
    // relationship is fine (because it's not yet established).
    if (!parent || descendant.isEmpty()) {
        parentFile_ = parent;
        file_ = descendant;
        pathLabel_ = formatPath();
        updateDisplay();
        return;
    }

    const std::optional<QString> descendantParent = path::parent(descendant);
    if (!descendantParent)
        throw PathError(QStringLiteral("Path %1 has no parent").arg(descendant).toStdString());

    const bool descendantOrEqual = path::normalize(*parent) == path::normalize(*descendantParent)
        || path::relativePath(*parent, *descendantParent).has_value();
    if (!descendantOrEqual) {
        throw PathError(
            QStringLiteral("Path %1 is not a descendant of %2").arg(descendant, *parent).toStdString());
    }

    parentFile_ = parent;
    file_ = descendant;
    pathLabel_ = formatPath();
    updateDisplay();
}

void PathLabel::updateDisplay()
{
    setToolTip(file_.isEmpty() ? QString() : QDir::toNativeSeparators(file_));
    updateGeometry();
    update();
}

QSize PathLabel::sizeHint() const
{
    const QFontMetrics fm = fontMetrics();
    const int arrow = style()->pixelMetric(QStyle::PM_MenuButtonIndicator, nullptr, this);
    return QSize(fm.horizontalAdvance(displayLabel()) + arrow + 24, std::max(fm.height() + 12, 28));
}

QSize PathLabel::minimumSizeHint() const
{
    const QFontMetrics fm = fontMetrics();
    return QSize(fm.horizontalAdvance(QStringLiteral("…")) * 6, std::max(fm.height() + 12, 28));
}

void PathLabel::resizeEvent(QResizeEvent *event)
{
    QToolButton::resizeEvent(event);
    update();
}

void PathLabel::paintEvent(QPaintEvent *)
{
    QStylePainter painter(this);
    QStyleOptionToolButton option;
    initStyleOption(&option);
    option.text.clear();
    option.features &= ~QStyleOptionToolButton::HasMenu;
    painter.drawComplexControl(QStyle::CC_ToolButton, option);

    // Draw the label elided in the middle, followed by a drop-down arrow
    const int arrowSize = 10;
    const QRect content = rect().adjusted(8, 0, -8, 0);
    const QRect textRect = content.adjusted(0, 0, -(arrowSize + 6), 0);
    const QString elided = fontMetrics().elidedText(displayLabel(), Qt::ElideMiddle, textRect.width());
    const int textWidth = fontMetrics().horizontalAdvance(elided);
    const int total = textWidth + 6 + arrowSize;
    const int startX = content.left() + std::max(0, (content.width() - total) / 2);
    painter.setPen(palette().color(QPalette::ButtonText));
    painter.drawText(QRect(startX, 0, textWidth, height()), Qt::AlignVCenter | Qt::AlignLeft, elided);

    QStyleOption arrowOption;
    arrowOption.initFrom(this);
    arrowOption.rect = QRect(startX + textWidth + 6, (height() - arrowSize) / 2, arrowSize, arrowSize);
    painter.drawPrimitive(QStyle::PE_IndicatorArrowDown, arrowOption);
}

void PathLabel::showPopup()
{
    auto *menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);

    auto *content = new QWidget(menu);
    auto *layout = new QVBoxLayout(content);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(6);

    auto *pathRow = new QHBoxLayout;
    pathRow->setSpacing(12);
    auto *pathTitle = new QLabel(tr("Path"), content);
    pathTitle->setEnabled(false);
    pathRow->addWidget(pathTitle);
    auto *fullPath = new QLineEdit(content);
    fullPath->setReadOnly(true);
    fullPath->setFrame(false);
    fullPath->setText(QDir::toNativeSeparators(file_));
    fullPath->setMinimumWidth(fullPath->fontMetrics().averageCharWidth() * 60);
    pathRow->addWidget(fullPath, 1);
    layout->addLayout(pathRow);

    auto *buttons = new QHBoxLayout;
    buttons->setSpacing(0);
    buttons->addStretch();
    auto *copyButton = new QPushButton(tr("Copy Path"), content);
    copyButton->setToolTip(tr("Copy the full path"));
    copyButton->setEnabled(!file_.isEmpty());
    auto *openButton = new QPushButton(tr("Open Containing Folder"), content);
    openButton->setToolTip(tr("View the folder in file manager"));
    openButton->setEnabled(!file_.isEmpty());
    buttons->addWidget(copyButton);
    buttons->addWidget(openButton);
    layout->addLayout(buttons);

    connect(copyButton, &QPushButton::clicked, menu, [this, menu]() {
        QApplication::clipboard()->setText(QDir::toNativeSeparators(file_));
        menu->close();
    });
    connect(openButton, &QPushButton::clicked, menu, [this, menu]() {
        openContainingFolder(file_);
        menu->close();
    });

    auto *action = new QWidgetAction(menu);
    action->setDefaultWidget(content);
    menu->addAction(action);
    menu->popup(mapToGlobal(QPoint(0, height())));
    fullPath->setFocus();
    fullPath->selectAll();
}

FileButton::FileButton(int pane, Mode mode, const QString &dialogLabel, QWidget *parent)
    : QToolButton(parent)
    , pane_(pane)
    , mode_(mode)
    , dialogLabel_(dialogLabel)
{
    setIcon(icons::icon(mode == Mode::OpenFile ? QStringLiteral("document-open-symbolic")
                                               : QStringLiteral("folder-open-symbolic")));
    setAutoRaise(true);
    connect(this, &QToolButton::clicked, this, &FileButton::chooseFile);
}

void FileButton::chooseFile()
{
    auto *dialog = new QFileDialog(window(), dialogLabel_);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    if (mode_ == Mode::SelectFolder) {
        dialog->setFileMode(QFileDialog::Directory);
        dialog->setOption(QFileDialog::ShowDirsOnly);
    } else {
        dialog->setFileMode(QFileDialog::ExistingFile);
    }
    if (!file_.isEmpty()) {
        const QFileInfo info(file_);
        dialog->setDirectory(info.isDir() && mode_ == Mode::SelectFolder ? info.absolutePath()
                                                                          : info.absolutePath());
        dialog->selectFile(info.fileName());
    }
    connect(dialog, &QFileDialog::fileSelected, this, [this](const QString &file) {
        if (file.isEmpty())
            return;
        file_ = QDir::cleanPath(file);
        Q_EMIT fileSelected(pane_, file_);
    });
    dialog->open();
}

} // namespace meld
