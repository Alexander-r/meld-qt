#pragma once

#include <QObject>
#include <QStringList>

#include <memory>
#include <optional>

namespace meld::archive {

//! Content types that should probably be opened as archives
const QStringList &archiveContentTypes();

//! Whether a path is a regular file with an archive content type
bool isArchive(const QString &path);

//! An installed program, and its arguments, that extracts an archive
struct Extractor {
    QString program;
    QStringList arguments;
    //! The highest exit status that isn't a failure (e.g., warnings)
    int maxSuccessStatus = 0;
};

/*!
 * How to extract an archive into a folder, with the first installed program
 * that reads its format; nullopt if there's none
 */
std::optional<Extractor> extractorFor(const QString &archive, const QString &destination);

//! Extracts an archive, in the background
class Extraction : public QObject {
    Q_OBJECT

public:
    explicit Extraction(const QString &archive, QObject *parent = nullptr);
    //! Cancels the extraction, if it's running
    ~Extraction() override;

    QString archive() const;

    //! Start extracting; finished() or failed() follows
    void start();
    //! Stop extracting, and remove what was extracted; no signal follows
    void cancel();
    bool isRunning() const;

Q_SIGNALS:
    //! The archive was extracted to `root`, now an extracted archive root
    void finished(const QString &root);
    void failed(const QString &error);

private:
    void fail(const QString &error);
    void processFinished(int exitCode, bool crashed);

    struct Private;
    std::unique_ptr<Private> d_;
};

//! Whether any archives are extracted (have_active_mounts)
bool haveExtractedArchives();
//! The archive that a path was extracted from, if it's in an extracted archive
std::optional<QString> archiveForPath(const QString &path);
//! The archive that a path was extracted from, or an empty string
QString formatArchivePath(const QString &path);
//! Whether a path is an extracted archive's root (is_mounted_archive_root)
bool isExtractedArchiveRoot(const QString &path);
//! A path in an extracted archive as "archive.zip — folder/file"
//! (format_mount_relative_path), or an empty string if it isn't in one
QString formatArchiveRelativePath(const QString &path);
//! A path in an extracted archive as if the archive were a folder (e.g.,
//! "/path/archive.zip/folder/file"), or an empty string if it isn't in one
QString formatArchiveFilePath(const QString &path);
//! Remove extracted archives by their roots (unmount_archives_by_file)
void removeExtractedArchives(const QStringList &roots);
//! Remove every extracted archive (unmount_archives)
void removeAllExtractedArchives();

} // namespace meld::archive
