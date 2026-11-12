#include "archive.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QLoggingCategory>
#include <QMimeDatabase>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <functional>
#include <vector>

namespace meld::archive {

// As in the original, information is only logged when asked for
Q_LOGGING_CATEGORY(lcArchive, "meld.archive", QtWarningMsg)

namespace {

//! What's kept of a failed extraction's error output
constexpr qsizetype MaxErrorOutput = 64 * 1024;
constexpr int ErrorLines = 10;

const QStringList &tarContentTypes()
{
    static const QStringList types{
        QStringLiteral("application/x-bzip-compressed-tar"), QStringLiteral("application/x-compressed-tar"),
        QStringLiteral("application/x-lzma-compressed-tar"), QStringLiteral("application/x-tar"),
        QStringLiteral("application/x-tarz"),                QStringLiteral("application/x-xz-compressed-tar"),
    };
    return types;
}

const QStringList &zipContentTypes()
{
    static const QStringList types{
        QStringLiteral("application/java-archive"), QStringLiteral("application/vnd.android.package-archive"),
        QStringLiteral("application/x-zip"),        QStringLiteral("application/x-zip-compressed"),
        QStringLiteral("application/zip"),
    };
    return types;
}

//! What 7-Zip extracts as the files in them (it extracts the tar file from
//! a compressed tar, and an RPM's payload)
const QStringList &sevenZipContentTypes()
{
    static const QStringList types = zipContentTypes()
        + QStringList{
            QStringLiteral("application/vnd.ms-cab-compressed"),
            QStringLiteral("application/vnd.rar"),
            QStringLiteral("application/x-7z-compressed"),
            QStringLiteral("application/x-archive"),
            QStringLiteral("application/x-cd-image"),
            QStringLiteral("application/x-cpio"),
            QStringLiteral("application/x-deb"),
            QStringLiteral("application/x-iso9660-image"),
            QStringLiteral("application/x-lha"),
            QStringLiteral("application/x-lzh-compressed"),
            QStringLiteral("application/x-rar"),
            QStringLiteral("application/x-rar-compressed"),
            QStringLiteral("application/x-xar"),
        };
    return types;
}

//! The content type of a file, and its aliases. The original's list has some
//! aliases, which newer MIME databases don't give as the type (e.g.,
//! application/x-rar is now application/vnd.rar).
QStringList contentTypes(const QString &path)
{
    const QMimeType mime = QMimeDatabase().mimeTypeForFile(path);
    return QStringList{mime.name()} + mime.aliases();
}

bool hasType(const QStringList &types, const QStringList &among)
{
    for (const QString &type : types) {
        if (among.contains(type))
            return true;
    }
    return false;
}

QString findProgram(const QString &name)
{
    QString program = QStandardPaths::findExecutable(name);
#ifdef Q_OS_WIN
    if (program.isEmpty() && name.startsWith(QLatin1String("7z"))) {
        QStringList folders;
        for (const char *variable : {"ProgramFiles", "ProgramW6432", "ProgramFiles(x86)"}) {
            if (const QString folder = qEnvironmentVariable(variable); !folder.isEmpty())
                folders.append(QDir(folder).filePath(QStringLiteral("7-Zip")));
        }
        program = QStandardPaths::findExecutable(name, folders);
    }
#endif
    return program;
}

//! Whether a tar program is bsdtar (as on BSDs, macOS and Windows), which
//! reads every format, rather than GNU tar
bool isBsdtar(const QString &program)
{
    static QHash<QString, bool> known;
    if (const auto it = known.constFind(program); it != known.cend())
        return *it;
    QProcess process;
    process.setStandardInputFile(QProcess::nullDevice());
    process.start(program, {QStringLiteral("--version")});
    bool bsdtar = false;
    if (process.waitForFinished(3000))
        bsdtar = process.readAllStandardOutput().contains("bsdtar");
    else
        process.kill();
    known.insert(program, bsdtar);
    return bsdtar;
}

//! Make a path's permissions include some, and exclude others
void adjustPermissions(const QString &path, QFile::Permissions add, QFile::Permissions remove = {})
{
    const QFile::Permissions current = QFile::permissions(path);
    const QFile::Permissions wanted = (current | add) & ~remove;
    if (wanted != current)
        QFile::setPermissions(path, wanted);
}

constexpr QFile::Permissions WritePermissions =
    QFile::WriteOwner | QFile::WriteUser | QFile::WriteGroup | QFile::WriteOther;

bool isLink(const QFileInfo &info)
{
    return info.isSymLink() || info.isJunction();
}

/*!
 * Prepare an extracted folder for comparing: folders are made read-only, as
 * archives can't be changed, and files readable. Special files (e.g.,
 * FIFOs, which would block reading) are removed. Links aren't followed.
 */
void prepareExtracted(const QString &folder)
{
    adjustPermissions(folder, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    const QFileInfoList entries =
        QDir(folder).entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    for (const QFileInfo &entry : entries) {
        if (isLink(entry))
            continue;
        if (entry.isDir())
            prepareExtracted(entry.absoluteFilePath());
        else if (entry.isFile())
            adjustPermissions(entry.absoluteFilePath(), QFile::ReadOwner);
        else
            QFile::remove(entry.absoluteFilePath());
    }
    adjustPermissions(folder, QFile::ReadOwner | QFile::ExeOwner, WritePermissions);
}

//! Remove a folder made by an extraction, read-only parts and all
void removeTree(const QString &folder)
{
    const std::function<void(const QString &)> makeWritable = [&makeWritable](const QString &path) {
        adjustPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        const QFileInfoList entries =
            QDir(path).entryInfoList(QDir::AllDirs | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
        for (const QFileInfo &entry : entries) {
            if (!isLink(entry))
                makeWritable(entry.absoluteFilePath());
        }
    };
    if (folder.isEmpty() || !QFileInfo(folder).isDir())
        return;
    makeWritable(folder);
    if (!QDir(folder).removeRecursively())
        qCWarning(lcArchive) << "Couldn't remove the extracted archive" << folder;
}

struct Extracted {
    //! The temporary folder, which has the root
    QString container;
    //! The extracted files, in a folder named as the archive
    QString root;
    QString archive;
};

//! The extracted archives (_archive_mounts); they're removed when Meld ends
class Registry {
public:
    // The logging category is used when the registry goes, so it must be
    // made before it
    Registry() { lcArchive(); }
    ~Registry() { removeAll(); }

    static Registry &instance()
    {
        static Registry registry;
        return registry;
    }

    std::vector<Extracted> entries;

    const Extracted *find(const QString &path) const
    {
        if (path.isEmpty())
            return nullptr;
        const QString clean = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
        for (const Extracted &entry : entries) {
            if (clean == entry.root || clean.startsWith(entry.root + QLatin1Char('/')))
                return &entry;
        }
        return nullptr;
    }

    void remove(const QString &root)
    {
        const QString clean = QDir::cleanPath(QFileInfo(root).absoluteFilePath());
        for (auto it = entries.begin(); it != entries.end(); ++it) {
            if (it->root == clean) {
                qCInfo(lcArchive) << "Removing the extracted archive" << it->archive;
                removeTree(it->container);
                entries.erase(it);
                return;
            }
        }
        qCWarning(lcArchive) << "Tried to remove unknown extracted archive" << root;
    }

    void removeAll()
    {
        while (!entries.empty())
            remove(entries.back().root);
    }
};

} // namespace

const QStringList &archiveContentTypes()
{
    // Content types that the gvfs archive:// mount should probably handle
    static const QStringList types{
        QStringLiteral("application/java-archive"),
        QStringLiteral("application/vnd.android.package-archive"),
        QStringLiteral("application/vnd.ms-cab-compressed"),
        QStringLiteral("application/x-7z-compressed"),
        QStringLiteral("application/x-archive"),
        QStringLiteral("application/x-bzip-compressed-tar"),
        QStringLiteral("application/x-cd-image"),
        QStringLiteral("application/x-compressed-tar"),
        QStringLiteral("application/x-cpio"),
        QStringLiteral("application/x-deb"),
        QStringLiteral("application/x-iso9660-image"),
        QStringLiteral("application/x-lha"),
        QStringLiteral("application/x-lzh-compressed"),
        QStringLiteral("application/x-lzma-compressed-tar"),
        QStringLiteral("application/x-rar"),
        QStringLiteral("application/x-rar-compressed"),
        QStringLiteral("application/x-rpm"),
        QStringLiteral("application/x-stuffit"),
        QStringLiteral("application/x-tar"),
        QStringLiteral("application/x-tarz"),
        QStringLiteral("application/x-xar"),
        QStringLiteral("application/x-xz-compressed-tar"),
        QStringLiteral("application/x-zip"),
        QStringLiteral("application/x-zip-compressed"),
        QStringLiteral("application/zip"),
    };
    return types;
}

bool isArchive(const QString &path)
{
    if (path.isEmpty())
        return false;
    const QFileInfo info(path);
    if (!info.isFile())
        return false;
    return hasType(contentTypes(path), archiveContentTypes());
}

std::optional<Extractor> extractorFor(const QString &archive, const QString &destination)
{
    const QStringList types = contentTypes(archive);
    const QString source = QDir::toNativeSeparators(archive);
    const QString target = QDir::toNativeSeparators(destination);

    // bsdtar reads every format, as gvfs does with libarchive. Its defaults
    // are safe: it refuses paths with "..", absolute paths, and extracting
    // through links.
    const auto bsdtar = [&](const QString &program) {
        return Extractor{program, {QStringLiteral("-x"), QStringLiteral("-f"), source, QStringLiteral("-C"), target}};
    };
    if (const QString program = findProgram(QStringLiteral("bsdtar")); !program.isEmpty())
        return bsdtar(program);
    const QString tar = findProgram(QStringLiteral("tar"));
    if (!tar.isEmpty() && isBsdtar(tar))
        return bsdtar(tar);

    if (hasType(types, sevenZipContentTypes())) {
        for (const char *name : {"7z", "7zz", "7za"}) {
            const QString program = findProgram(QLatin1String(name));
            if (program.isEmpty())
                continue;
            // Status 1 is for warnings, e.g., about skipped unsafe paths
            return Extractor{program,
                             {QStringLiteral("x"), QStringLiteral("-y"), QStringLiteral("-bd"),
                              QStringLiteral("-o") + target, QStringLiteral("--"), source},
                             1};
        }
    }
    if (hasType(types, tarContentTypes()) && !tar.isEmpty()) {
        // GNU tar skips members with "..", and strips leading slashes. A
        // colon would otherwise make it a remote archive.
        return Extractor{tar,
                         {QStringLiteral("--force-local"), QStringLiteral("--no-same-owner"), QStringLiteral("-x"),
                          QStringLiteral("-f"), source, QStringLiteral("-C"), target}};
    }
    if (hasType(types, zipContentTypes())) {
        if (const QString program = findProgram(QStringLiteral("unzip")); !program.isEmpty()) {
            // Status 1 is for warnings
            return Extractor{program, {QStringLiteral("-q"), QStringLiteral("-o"), source, QStringLiteral("-d"), target},
                             1};
        }
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Extraction

struct Extraction::Private {
    QString archive;
    QString container;
    QString root;
    QProcess *process = nullptr;
    QByteArray errorOutput;
    int maxSuccessStatus = 0;
};

Extraction::Extraction(const QString &archive, QObject *parent)
    : QObject(parent)
    , d_(std::make_unique<Private>())
{
    d_->archive = QDir::cleanPath(QFileInfo(archive).absoluteFilePath());
}

Extraction::~Extraction()
{
    cancel();
}

QString Extraction::archive() const
{
    return d_->archive;
}

bool Extraction::isRunning() const
{
    return d_->process != nullptr;
}

void Extraction::start()
{
    if (isRunning())
        return;
    const QString name = QFileInfo(d_->archive).fileName();

    QTemporaryDir container(QDir(QDir::tempPath()).filePath(QStringLiteral("meld-qt-archive-XXXXXX")));
    if (!container.isValid()) {
        fail(container.errorString());
        return;
    }
    container.setAutoRemove(false);
    d_->container = QDir::cleanPath(container.path());
    // The root is named as the archive, which then shows in shortened paths
    d_->root = QDir(d_->container).filePath(name);
    if (!QDir().mkdir(d_->root)) {
        fail(QCoreApplication::translate("Extraction", "Couldn’t create folder %1").arg(d_->root));
        return;
    }

    const std::optional<Extractor> extractor = extractorFor(d_->archive, d_->root);
    if (!extractor) {
        fail(QCoreApplication::translate("Extraction",
                                         "No installed program can extract this archive. Install bsdtar "
                                         "(from libarchive) or 7-Zip."));
        return;
    }
    d_->maxSuccessStatus = extractor->maxSuccessStatus;
    d_->errorOutput.clear();

    auto *process = new QProcess(this);
    d_->process = process;
    process->setProgram(extractor->program);
    process->setArguments(extractor->arguments);
    process->setWorkingDirectory(d_->root);
    // Programs that ask for passwords get none, rather than waiting for them
    process->setStandardInputFile(QProcess::nullDevice());
    process->setStandardOutputFile(QProcess::nullDevice());
#ifdef Q_OS_UNIX
    // ...including those that ask on the terminal that Meld was started from
    process->setUnixProcessParameters(QProcess::UnixProcessFlag::CreateNewSession);
#endif
    connect(process, &QProcess::readyReadStandardError, this, [this, process]() {
        d_->errorOutput += process->readAllStandardError();
        if (d_->errorOutput.size() > MaxErrorOutput)
            d_->errorOutput = d_->errorOutput.right(MaxErrorOutput);
    });
    connect(process, &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus status) {
        processFinished(exitCode, status == QProcess::CrashExit);
    });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            fail(process->errorString());
    });
    qCInfo(lcArchive) << "Extracting" << d_->archive << "with" << extractor->program;
    process->start();
}

void Extraction::processFinished(int exitCode, bool crashed)
{
    if (!d_->process)
        return;
    d_->errorOutput += d_->process->readAllStandardError();
    const QString program = QFileInfo(d_->process->program()).fileName();
    d_->process->deleteLater();
    d_->process = nullptr;

    if (crashed || exitCode > d_->maxSuccessStatus) {
        QStringList lines = QString::fromLocal8Bit(d_->errorOutput).trimmed().split(QLatin1Char('\n'));
        lines.removeAll(QString());
        if (lines.size() > ErrorLines)
            lines = lines.mid(lines.size() - ErrorLines);
        QString message = lines.join(QLatin1Char('\n')).trimmed();
        if (message.isEmpty()) {
            message = crashed ? QCoreApplication::translate("Extraction", "%1 crashed").arg(program)
                              : QCoreApplication::translate("Extraction", "%1 failed with status %2")
                                    .arg(program)
                                    .arg(exitCode);
        }
        fail(message);
        return;
    }

    prepareExtracted(d_->root);
    Registry::instance().entries.push_back({d_->container, d_->root, d_->archive});
    const QString root = d_->root;
    d_->container.clear();
    d_->root.clear();
    Q_EMIT finished(root);
}

void Extraction::fail(const QString &error)
{
    if (d_->process) {
        d_->process->disconnect(this);
        d_->process->kill();
        d_->process->waitForFinished();
        d_->process->deleteLater();
        d_->process = nullptr;
    }
    removeTree(d_->container);
    d_->container.clear();
    d_->root.clear();
    qCWarning(lcArchive) << "Failed to extract" << d_->archive << ":" << error;
    Q_EMIT failed(error);
}

void Extraction::cancel()
{
    if (d_->process) {
        d_->process->disconnect(this);
        d_->process->kill();
        d_->process->waitForFinished();
        delete d_->process;
        d_->process = nullptr;
    }
    removeTree(d_->container);
    d_->container.clear();
    d_->root.clear();
}

// ---------------------------------------------------------------------------
// Extracted archives

bool haveExtractedArchives()
{
    return !Registry::instance().entries.empty();
}

std::optional<QString> archiveForPath(const QString &path)
{
    if (const Extracted *entry = Registry::instance().find(path))
        return entry->archive;
    return std::nullopt;
}

QString formatArchivePath(const QString &path)
{
    return archiveForPath(path).value_or(QString());
}

bool isExtractedArchiveRoot(const QString &path)
{
    const Extracted *entry = Registry::instance().find(path);
    return entry && entry->root == QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

QString formatArchiveRelativePath(const QString &path)
{
    const Extracted *entry = Registry::instance().find(path);
    if (!entry)
        return {};
    const QString name = QFileInfo(entry->archive).fileName();
    const QString relative = QDir(entry->root).relativeFilePath(QDir::cleanPath(QFileInfo(path).absoluteFilePath()));
    if (relative.isEmpty() || relative == QLatin1String("."))
        return name;
    return QStringLiteral("%1 — %2").arg(name, relative);
}

QString formatArchiveFilePath(const QString &path)
{
    const Extracted *entry = Registry::instance().find(path);
    if (!entry)
        return {};
    const QString relative = QDir(entry->root).relativeFilePath(QDir::cleanPath(QFileInfo(path).absoluteFilePath()));
    if (relative.isEmpty() || relative == QLatin1String("."))
        return entry->archive;
    return entry->archive + QLatin1Char('/') + relative;
}

void removeExtractedArchives(const QStringList &roots)
{
    for (const QString &root : roots)
        Registry::instance().remove(root);
}

void removeAllExtractedArchives()
{
    Registry::instance().removeAll();
}

} // namespace meld::archive
