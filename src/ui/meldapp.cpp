#include "meldapp.h"

#include "filediff.h"
#include "meldwindow.h"
#include "preferences.h"
#include "recent.h"
#ifdef MELD_SINGLE_INSTANCE
#include "singleinstance.h"
#endif

#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QLabel>
#include <QUrl>
#include <QVBoxLayout>

#include <cstdio>

namespace meld {

namespace {

struct OptionSpec {
    QString shortName;
    QString longName;
    QString metavar;
    QString help;
};

QList<OptionSpec> optionSpecs()
{
    return {
        {QString(), QStringLiteral("version"), QString(), QObject::tr("show program's version number and exit")},
        {QStringLiteral("h"), QStringLiteral("help"), QString(), QObject::tr("show this help message and exit")},
        {QStringLiteral("L"), QStringLiteral("label"), QStringLiteral("LABEL"),
         QObject::tr("Set label to use instead of file name")},
#ifdef MELD_SINGLE_INSTANCE
        {QStringLiteral("n"), QStringLiteral("newtab"), QString(),
         QObject::tr("Open a new tab in an already running instance")},
        {QString(), QStringLiteral("single-instance"), QString(),
         QObject::tr("Open comparisons in an already running instance (single-instance mode)")},
        {QString(), QStringLiteral("no-single-instance"), QString(),
         QObject::tr("Open comparisons in a new instance, even in single-instance mode")},
#else
        {QStringLiteral("n"), QStringLiteral("newtab"), QString(), QObject::tr("Ignored for compatibility")},
#endif
        {QStringLiteral("a"), QStringLiteral("auto-compare"), QString(),
         QObject::tr("Automatically compare all differing files on startup")},
        {QStringLiteral("u"), QStringLiteral("unified"), QString(), QObject::tr("Ignored for compatibility")},
        {QStringLiteral("o"), QStringLiteral("output"), QStringLiteral("OUTFILE"),
         QObject::tr("Set the target file for saving a merge result")},
        {QString(), QStringLiteral("auto-merge"), QString(), QObject::tr("Automatically merge files")},
        {QString(), QStringLiteral("comparison-file"), QStringLiteral("COMPARISON_FILE"),
         QObject::tr("Load a saved comparison from a Meld Qt comparison file")},
        {QString(), QStringLiteral("patch"), QStringLiteral("PATCH"),
         QObject::tr("Compare a file or folder with what a patch would make of it")},
        {QString(), QStringLiteral("diff"), QString(),
         QObject::tr("Create a diff tab for the supplied files or folders")},
    };
}

class ParseError : public std::exception {
public:
    explicit ParseError(QString message)
        : message(std::move(message))
    {
    }
    QString message;
};

bool looksLikeOption(const QString &arg)
{
    // Stop if we find a short- or long-form arg, or a '--'
    return arg.startsWith(QLatin1String("--")) || (arg.startsWith(QLatin1Char('-')) && arg.size() > 1);
}

QString expandUser(const QString &path)
{
    if (path == QLatin1String("~"))
        return QDir::homePath();
    if (path.startsWith(QLatin1String("~/")))
        return QDir::homePath() + path.mid(1);
    return path;
}

//! Resolve a command line argument to an absolute local path, relative to
//! the invocation's working directory; empty for the special @blank
//! argument.
QString fileFromCommandLine(const QString &arg, const QString &cwd)
{
    if (arg.startsWith(QLatin1String("file:"))) {
        const QUrl url = QUrl::fromUserInput(arg);
        if (url.isLocalFile())
            return url.toLocalFile();
    }
    const QFileInfo info(QDir(cwd), arg);
    if (!info.exists() && arg == QLatin1String("@blank"))
        return QString();
    return QDir::cleanPath(info.absoluteFilePath());
}

void printText(const QString &text, FILE *stream)
{
    const QByteArray bytes = text.toLocal8Bit();
    std::fwrite(bytes.constData(), 1, size_t(bytes.size()), stream);
    std::fflush(stream);
}

} // namespace

QString commandLineUsage(const QString &programName)
{
    const QList<QPair<QString, QString>> usages = {
        {QString(), QObject::tr("Start with an empty window")},
        {QStringLiteral("<%1|%2>").arg(QObject::tr("file"), QObject::tr("folder")),
         QObject::tr("Start a comparison against a blank pane")},
        {QStringLiteral("<%1> <%1> [<%1>]").arg(QObject::tr("file")), QObject::tr("Start a 2- or 3-way file comparison")},
        {QStringLiteral("<%1> <%1> [<%1>]").arg(QObject::tr("folder")),
         QObject::tr("Start a 2- or 3-way folder comparison")},
        {QStringLiteral("--patch <%1> <%2|%3>").arg(QObject::tr("patch"), QObject::tr("file"), QObject::tr("folder")),
         QObject::tr("Compare with a patch applied")},
    };
    qsizetype width = 0;
    for (const auto &[args, description] : usages)
        width = std::max(width, args.size());

    QString usage = QObject::tr("Usage:") + QStringLiteral(" \n");
    for (const auto &[args, description] : usages)
        usage += QStringLiteral("  %1 %2 %3\n").arg(programName, args.leftJustified(width), description);
    usage += QObject::tr("\n"
                         "For file comparisons, the special argument @blank may be used instead of\n"
                         "a <file> argument to create a blank pane.");
    return usage + QLatin1Char('\n');
}

QString commandLineHelp(const QString &programName)
{
    QString help = commandLineUsage(programName);
    help += QLatin1Char('\n') + QObject::tr("Meld Qt is a file and directory comparison tool.") + QStringLiteral("\n\n");
    help += QObject::tr("Options:") + QLatin1Char('\n');

    // optparse's IndentedHelpFormatter layout
    constexpr int helpPosition = 24;
    constexpr int optionWidth = helpPosition - 2 - 2;
    for (const OptionSpec &spec : optionSpecs()) {
        QStringList names;
        if (!spec.shortName.isEmpty())
            names.append(spec.metavar.isEmpty() ? QStringLiteral("-") + spec.shortName
                                                : QStringLiteral("-%1 %2").arg(spec.shortName, spec.metavar));
        names.append(spec.metavar.isEmpty() ? QStringLiteral("--") + spec.longName
                                            : QStringLiteral("--%1=%2").arg(spec.longName, spec.metavar));
        const QString options = names.join(QStringLiteral(", "));
        if (options.size() > optionWidth)
            help += QStringLiteral("  %1\n%2%3\n").arg(options, QString(helpPosition, QLatin1Char(' ')), spec.help);
        else
            help += QStringLiteral("  %1  %2\n").arg(options.leftJustified(optionWidth), spec.help);
    }
    return help;
}

CommandLineParseResult parseCommandLine(const QStringList &arguments, const QString &programName, const QString &cwd)
{
    CommandLineParseResult result;
    CommandLineOptions &options = result.options;
    const QList<OptionSpec> specs = optionSpecs();

    auto findLong = [&](const QString &name) -> const OptionSpec & {
        // optparse accepts unambiguous prefixes of long options
        const OptionSpec *found = nullptr;
        QStringList candidates;
        for (const OptionSpec &spec : specs) {
            if (spec.longName == name)
                return spec;
            if (spec.longName.startsWith(name)) {
                found = &spec;
                candidates.append(QStringLiteral("--") + spec.longName);
            }
        }
        if (candidates.size() == 1)
            return *found;
        if (candidates.isEmpty())
            throw ParseError(QObject::tr("no such option: %1").arg(QStringLiteral("--") + name));
        throw ParseError(QObject::tr("ambiguous option: %1 (%2?)")
                             .arg(QStringLiteral("--") + name, candidates.join(QStringLiteral(", "))));
    };
    auto findShort = [&](const QString &name) -> const OptionSpec & {
        for (const OptionSpec &spec : specs) {
            if (spec.shortName == name)
                return spec;
        }
        throw ParseError(QObject::tr("no such option: %1").arg(QStringLiteral("-") + name));
    };

    try {
        qsizetype i = 0;
        // Returns true if processing should stop (help or version)
        auto apply = [&](const OptionSpec &spec, const QString &optionText, const std::optional<QString> &value) {
            if (!spec.metavar.isEmpty() && !value)
                throw ParseError(QObject::tr("%1 option requires an argument").arg(optionText));
            if (spec.metavar.isEmpty() && value)
                throw ParseError(QObject::tr("%1 option does not take a value").arg(optionText));

            const QString &name = spec.longName;
            if (name == QLatin1String("help")) {
                result.shouldExit = true;
                result.output = commandLineHelp(programName) + QLatin1Char('\n');
            } else if (name == QLatin1String("version")) {
                result.shouldExit = true;
                result.output = QStringLiteral("%1 %2\n\n").arg(programName, QStringLiteral(MELD_VERSION));
            } else if (name == QLatin1String("label")) {
                options.labels.append(*value);
            } else if (name == QLatin1String("newtab")) {
                options.newTab = true;
            } else if (name == QLatin1String("single-instance")) {
                options.singleInstance = true;
            } else if (name == QLatin1String("no-single-instance")) {
                options.singleInstance = false;
            } else if (name == QLatin1String("auto-compare")) {
                options.autoCompare = true;
            } else if (name == QLatin1String("unified")) {
                options.unified = true;
            } else if (name == QLatin1String("output")) {
                options.outFile = *value;
            } else if (name == QLatin1String("auto-merge")) {
                options.autoMerge = true;
            } else if (name == QLatin1String("comparison-file")) {
                options.comparisonFile = *value;
            } else if (name == QLatin1String("patch")) {
                options.patch = *value;
            } else if (name == QLatin1String("diff")) {
                // Gather --diff arguments
                QStringList diffArgs;
                while (i < arguments.size() && !looksLikeOption(arguments[i]))
                    diffArgs.append(arguments[i++]);
                if (diffArgs.isEmpty() || diffArgs.size() > 3)
                    throw ParseError(QObject::tr("wrong number of arguments supplied to --diff"));
                options.diffs.append(diffArgs);
            }
            return result.shouldExit;
        };

        while (i < arguments.size()) {
            const QString arg = arguments[i++];
            if (arg == QLatin1String("--")) {
                options.args.append(arguments.mid(i));
                break;
            }
            if (arg.startsWith(QLatin1String("--"))) {
                const qsizetype equals = arg.indexOf(QLatin1Char('='));
                const QString name = arg.mid(2, equals < 0 ? -1 : equals - 2);
                const OptionSpec &spec = findLong(name);
                const QString optionText = QStringLiteral("--") + spec.longName;
                std::optional<QString> value;
                if (equals >= 0) {
                    value = arg.mid(equals + 1);
                } else if (!spec.metavar.isEmpty()) {
                    if (i >= arguments.size())
                        throw ParseError(QObject::tr("%1 option requires an argument").arg(optionText));
                    value = arguments[i++];
                }
                if (apply(spec, optionText, value))
                    return result;
            } else if (arg.startsWith(QLatin1Char('-')) && arg.size() > 1) {
                // Short options may be grouped, and may have attached values
                for (qsizetype j = 1; j < arg.size(); ++j) {
                    const OptionSpec &spec = findShort(arg.mid(j, 1));
                    const QString optionText = QStringLiteral("-") + spec.shortName;
                    std::optional<QString> value;
                    if (!spec.metavar.isEmpty()) {
                        if (j + 1 < arg.size()) {
                            value = arg.mid(j + 1);
                        } else {
                            if (i >= arguments.size())
                                throw ParseError(QObject::tr("%1 option requires an argument").arg(optionText));
                            value = arguments[i++];
                        }
                        j = arg.size();
                    }
                    if (apply(spec, optionText, value))
                        return result;
                }
            } else {
                options.args.append(arg);
            }
        }

        if (!options.patch.isEmpty()) {
            if (options.args.size() != 1)
                throw ParseError(QObject::tr("--patch needs one file or folder to apply the patch to (got %1)")
                                     .arg(options.args.size()));
            if (options.autoMerge)
                throw ParseError(QObject::tr("can’t auto-merge with a patch"));
            if (!options.comparisonFile.isEmpty())
                throw ParseError(QObject::tr("can’t load a comparison file with a patch"));
        }
        if (options.args.size() > 3)
            throw ParseError(QObject::tr("too many arguments (wanted 0-3, got %1)").arg(options.args.size()));
        if (options.autoMerge && options.args.size() < 3)
            throw ParseError(QObject::tr("can’t auto-merge less than 3 files"));
        if (options.autoMerge) {
            for (const QString &arg : std::as_const(options.args)) {
                if (QFileInfo(QDir(cwd), arg).isDir())
                    throw ParseError(QObject::tr("can’t auto-merge directories"));
            }
        }
    } catch (const ParseError &error) {
        result.shouldExit = true;
        result.exitStatus = 2;
        result.output = commandLineUsage(programName) + QLatin1Char('\n')
            + QObject::tr("Error: %1\n").arg(error.message) + QLatin1Char('\n');
    }
    return result;
}

// ---------------------------------------------------------------------------

bool shouldForwardCommandLine(const CommandLineOptions &options, bool singleInstanceSetting)
{
#ifdef MELD_SINGLE_INSTANCE
    if (options.singleInstance)
        return *options.singleInstance;
    return singleInstanceSetting || (SingleInstanceServer::ReceivesWithoutSingleInstanceMode && options.newTab);
#else
    Q_UNUSED(options);
    Q_UNUSED(singleInstanceSetting);
    return false;
#endif
}

bool shouldReceiveCommandLines(const CommandLineOptions &options, bool singleInstanceSetting)
{
#ifdef MELD_SINGLE_INSTANCE
    return SingleInstanceServer::ReceivesWithoutSingleInstanceMode
        || options.singleInstance.value_or(singleInstanceSetting);
#else
    Q_UNUSED(options);
    Q_UNUSED(singleInstanceSetting);
    return false;
#endif
}

MeldApp::MeldApp(QObject *parent)
    : QObject(parent)
{
}

MeldApp::~MeldApp()
{
    for (const QPointer<MeldWindow> &window : std::as_const(windows_))
        delete window.data();
}

MeldWindow *MeldApp::window() const
{
    if (lastFocused_)
        return lastFocused_;
    for (qsizetype i = windows_.size() - 1; i >= 0; --i) {
        if (windows_.at(i))
            return windows_.at(i);
    }
    return nullptr;
}

QList<MeldWindow *> MeldApp::windows() const
{
    QList<MeldWindow *> result;
    for (const QPointer<MeldWindow> &window : windows_) {
        if (window)
            result.append(window);
    }
    return result;
}

MeldWindow *MeldApp::newWindow()
{
    auto *window = new MeldWindow();
    window->setAttribute(Qt::WA_DeleteOnClose);
    connect(window, &MeldWindow::preferencesRequested, this, &MeldApp::showPreferences);
    connect(window, &MeldWindow::aboutRequested, this, &MeldApp::showAbout);
    connect(window, &MeldWindow::quitRequested, this, &MeldApp::quit);
    // The most recently focused window gets new tabs (get_active_window())
    window->installEventFilter(this);
    windows_.removeAll(nullptr);
    windows_.append(window);
    return window;
}

bool MeldApp::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::WindowActivate) {
        if (auto *window = qobject_cast<MeldWindow *>(watched))
            lastFocused_ = window;
    }
    return QObject::eventFilter(watched, event);
}

int MeldApp::localError(const Invocation &invocation, const QString &message)
{
    invocation.print(commandLineUsage(programName_) + QLatin1Char('\n') + tr("Error: %1\n").arg(message)
                         + QLatin1Char('\n'),
                     true);
    return 2;
}

void MeldApp::activate(MeldWindow *window, const Invocation &invocation)
{
    window->show();
    // Window managers only let a window be raised for another program's
    // invocation with its activation token
    if (!invocation.activationToken.isEmpty())
        qputenv("XDG_ACTIVATION_TOKEN", invocation.activationToken);
    if (!invocation.startupId.isEmpty())
        qputenv("DESKTOP_STARTUP_ID", invocation.startupId);
    window->raise();
    window->activateWindow();
    if (invocation.remote) {
        qunsetenv("XDG_ACTIVATION_TOKEN");
        qunsetenv("DESKTOP_STARTUP_ID");
    }
}

int MeldApp::handleCommandLine(const QStringList &arguments)
{
    Invocation invocation;
    invocation.arguments = arguments;
    invocation.print = [](const QString &text, bool error) { printText(text, error ? stderr : stdout); };
    return handleInvocation(invocation);
}

#ifdef MELD_SINGLE_INSTANCE
bool MeldApp::startSingleInstanceServer(const QString &name)
{
    if (!server_) {
        server_ = new SingleInstanceServer(this);
        connect(server_, &SingleInstanceServer::commandLineReceived, this, &MeldApp::handleRemoteCommandLine);
    }
    return server_->listen(name);
}

void MeldApp::handleRemoteCommandLine(RemoteCommandLine *commandLine)
{
    QPointer<RemoteCommandLine> guard(commandLine);
    Invocation invocation;
    invocation.arguments = commandLine->arguments();
    invocation.cwd = commandLine->cwd().isEmpty() ? QDir::currentPath() : commandLine->cwd();
    invocation.remote = true;
    invocation.activationToken = commandLine->activationToken();
    invocation.startupId = commandLine->startupId();
    invocation.print = [guard](const QString &text, bool error) {
        if (guard)
            guard->print(text, error);
    };
    bool held = false;
    invocation.holdTab = [guard, &held](MeldDoc *tab) {
        // The invocation's exit status is the comparison's, when it's closed
        held = true;
        const auto done = [guard](int status) {
            if (guard) {
                guard->finish(status);
                guard->deleteLater();
            }
        };
        connect(tab, &MeldDoc::closed, guard.data(), done);
        connect(tab, &QObject::destroyed, guard.data(), [done]() { done(0); });
    };
    connect(commandLine, &RemoteCommandLine::disconnected, commandLine, &QObject::deleteLater);

    const int status = handleInvocation(invocation);
    if (guard && (status >= 0 || !held)) {
        guard->finish(std::max(status, 0));
        guard->deleteLater();
    }
}
#endif

int MeldApp::handleInvocation(const Invocation &invocation)
{
    const QStringList &arguments = invocation.arguments;
    if (!arguments.isEmpty())
        programName_ = QFileInfo(arguments.first()).fileName();

    const CommandLineParseResult parsed = parseCommandLine(arguments.mid(1), programName_, invocation.cwd);
    if (parsed.shouldExit) {
        invocation.print(parsed.output, parsed.exitStatus != 0);
        return parsed.exitStatus;
    }
    const CommandLineOptions &options = parsed.options;

    // Every invocation creates at most one window. Another invocation's
    // command line gets a new window unless it asks for a new tab, in which
    // case the most recently focused window is used.
    MeldWindow *window = invocation.remote && !options.newTab ? nullptr : this->window();
    const bool newWindowCreated = !window;
    if (!window)
        window = newWindow();
    // A window opened for the command line goes again if it fails
    const auto closeOnError = [window, newWindowCreated]() {
        if (newWindowCreated)
            delete window;
    };

    auto finalize = [this, window, &invocation]() {
        // Run after every comparison from this command line has started or failed
        if (!window->hasPages())
            window->appendNewComparison();
        activate(window, invocation);
    };

    if (!options.comparisonFile.isEmpty()
        || (options.patch.isEmpty() && options.args.size() == 1
            && options.args.first().endsWith(QLatin1String(".meldcmp")))) {
        QString path = expandUser(options.comparisonFile.isEmpty() ? options.args.first() : options.comparisonFile);
        path = QDir(invocation.cwd).absoluteFilePath(path);
        MeldDoc *tab = nullptr;
        try {
            tab = window->appendRecent(RecentComparisons::readComparisonFile(path));
        } catch (const std::exception &) {
            closeOnError();
            return localError(invocation, tr("Error reading saved comparison file"));
        }
        if (tab && invocation.holdTab)
            invocation.holdTab(tab);
        finalize();
        return -1;
    }

    QString error;
    bool haveComparison = false;

    QList<QStringList> comparisons;
    if (!options.args.isEmpty())
        comparisons.append(options.args);
    comparisons.append(options.diffs);

    for (qsizetype i = 0; i < comparisons.size(); ++i) {
        const bool isPrimary = i == 0;
        const bool autoMerge = options.autoMerge && isPrimary;

        QStringList files;
        for (const QString &path : comparisons[i])
            files.append(fileFromCommandLine(path, invocation.cwd));

        QString comparisonError;
        // The positional file or folder is compared with the patch applied
        MeldDoc *tab = isPrimary && !options.patch.isEmpty()
            ? window->openPatch(files.value(0), fileFromCommandLine(options.patch, invocation.cwd),
                                options.autoCompare, isPrimary, &comparisonError)
            : window->openPaths(files, options.autoCompare, autoMerge, isPrimary, &comparisonError);
        if (!comparisonError.isEmpty()) {
            error = comparisonError;
            continue;
        }
        if (!tab)
            continue;
        haveComparison = true;
        if (isPrimary) {
            if (!options.labels.isEmpty())
                tab->setLabels(options.labels);
            if (!options.outFile.isEmpty()) {
                if (auto *filediff = qobject_cast<FileDiff *>(tab))
                    filediff->setMergeOutputFile(fileFromCommandLine(options.outFile, invocation.cwd));
            }
            if (comparisons.size() == 1 && invocation.holdTab)
                invocation.holdTab(tab);
        }
    }

    if (!error.isEmpty()) {
        if (!haveComparison) {
            closeOnError();
            return localError(invocation, error);
        }
        invocation.print(error + QLatin1Char('\n'), true);
    }

    finalize();
    return -1;
}

void MeldApp::showPreferences()
{
    if (preferences_) {
        preferences_->raise();
        preferences_->activateWindow();
        return;
    }
    preferences_ = new PreferencesDialog(window());
    preferences_->setAttribute(Qt::WA_DeleteOnClose);
    preferences_->show();
}

void MeldApp::showAbout()
{
    auto *dialog = new QDialog(window());
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(tr("About Meld Qt"));
    dialog->setMinimumWidth(360);

    auto *layout = new QVBoxLayout(dialog);
    layout->setSpacing(6);
    layout->setContentsMargins(24, 24, 24, 12);

    auto *name = new QLabel(QStringLiteral("Meld Qt"), dialog);
    QFont nameFont = name->font();
    nameFont.setPointSizeF(nameFont.pointSizeF() * 1.8);
    nameFont.setBold(true);
    name->setFont(nameFont);
    name->setAlignment(Qt::AlignCenter);
    layout->addWidget(name);

    auto *version = new QLabel(QStringLiteral(MELD_VERSION), dialog);
    version->setAlignment(Qt::AlignCenter);
    layout->addWidget(version);

    auto *summary = new QLabel(tr("Visual diff and merge tool"), dialog);
    summary->setAlignment(Qt::AlignCenter);
    layout->addWidget(summary);

    auto *qtVersion = new QLabel(tr("Built with Qt %1").arg(QLatin1String(QT_VERSION_STR)), dialog);
    qtVersion->setAlignment(Qt::AlignCenter);
    layout->addWidget(qtVersion);

    layout->addSpacing(12);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog->open();
}

void MeldApp::quit()
{
    // Closing a window closes all of its tabs first; any tab may cancel this
    // (e.g., unsaved changes), in which case we keep running.
    const QList<MeldWindow *> open = windows();
    if (open.isEmpty()) {
        QApplication::quit();
        return;
    }
    for (MeldWindow *window : open) {
        if (!window->close())
            return;
    }
}

} // namespace meld
