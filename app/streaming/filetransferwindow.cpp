#include "filetransferwindow.h"

#include <algorithm>
#include <limits>

#include <QCoreApplication>
#include <QClipboard>
#include <QDir>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileInfo>
#include <QFileInfoList>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QHash>
#include <QImage>
#include <QInputMethod>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSaveFile>
#include <QScreen>
#include <QStorageInfo>
#include <QStyleHints>
#include <QUrl>
#include <QUuid>
#include <QWheelEvent>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

#include "filemappingprotocoladapter.h"
#include "settings/streamingpreferences.h"

namespace {
constexpr int kTransferTimeoutMs = 10000;
constexpr quint32 kTransferChunkBytes = 256U * 1024U;
constexpr int kMargin = 18;
constexpr int kHeaderHeight = 42;
constexpr int kPathHeight = 38;
constexpr int kRowsTop = 130;
constexpr int kStatusHeight = 48;
constexpr int kCenterWidth = 116;
constexpr int kColumnHeaderHeight = 30;
constexpr int kRowHeight = 36;
constexpr int kIconSize = 22;
constexpr int kActionCount = 4;

QString driveRootName(QString path)
{
    path = QDir::toNativeSeparators(path);
    while (path.size() > 1 &&
           (path.endsWith(QLatin1Char('\\')) ||
            path.endsWith(QLatin1Char('/')))) {
        path.chop(1);
    }
    return path;
}

QString localDriveDisplayName(const QString& path)
{
    const QString rootName = driveRootName(path);
    const QStorageInfo storage(path);
    const QString volumeName = storage.name().trimmed();
    if (volumeName.isEmpty() ||
        volumeName.compare(rootName, Qt::CaseInsensitive) == 0) {
        return rootName;
    }
    return QStringLiteral("%1 (%2)").arg(volumeName, rootName);
}

QString remoteDriveDisplayName(const QString& rootName,
                               const QString& volumeLabel)
{
    const QString label = volumeLabel.trimmed();
    if (label.isEmpty() ||
        label.compare(rootName, Qt::CaseInsensitive) == 0) {
        return rootName;
    }
    return QStringLiteral("%1 (%2)").arg(label, rootName);
}

QString remoteDriveRootName(const QString& mappingId,
                            const QString& displayName)
{
    const QString drivePrefix = QStringLiteral("drive-");
    if (mappingId.startsWith(drivePrefix) &&
        mappingId.size() == drivePrefix.size() + 1 &&
        mappingId.back().isLetter()) {
        return mappingId.back().toUpper() + QStringLiteral(":");
    }

    const int colon = displayName.lastIndexOf(QLatin1Char(':'));
    if (colon > 0 && displayName.at(colon - 1).isLetter()) {
        return displayName.mid(colon - 1, 2).toUpper();
    }
    return driveRootName(displayName);
}

FileMapping::ConflictPolicy conflictPolicyFromValue(int value)
{
    return value == static_cast<int>(StreamingPreferences::FTCP_OVERWRITE)
            ? FileMapping::ConflictPolicy::Overwrite
            : FileMapping::ConflictPolicy::KeepBoth;
}

QString uniqueLocalSiblingPath(const QString& requested)
{
    if (!QFileInfo::exists(requested)) {
        return requested;
    }

    const QFileInfo info(requested);
    const QString directory = info.absolutePath();
    const QString suffix = info.isDir() || info.suffix().isEmpty()
            ? QString()
            : QStringLiteral(".") + info.suffix();
    const QString base = info.isDir() || info.completeBaseName().isEmpty()
            ? info.fileName()
            : info.completeBaseName();
    for (int index = 1; index < std::numeric_limits<int>::max(); ++index) {
        const QString candidate = QDir(directory).filePath(
                QStringLiteral("%1 (%2)%3").arg(base).arg(index).arg(suffix));
        if (!QFileInfo::exists(candidate)) {
            return candidate;
        }
    }
    return QString();
}

QImage fileIcon(const QString& path,
                const QString& name,
                bool directory,
                bool drive,
                bool remote)
{
    static QHash<QString, QImage> cache;
    QString cacheKey;
    if (drive) {
        const QString rootName = driveRootName(path);
        cacheKey = QStringLiteral("drive:%1").arg(rootName.left(1).toUpper());
    }
    else if (directory) {
        cacheKey = QStringLiteral("folder");
    }
    else {
        const QString suffix = QFileInfo(name).suffix().toLower();
        if (!remote &&
            (suffix == QStringLiteral("exe") ||
             suffix == QStringLiteral("ico") ||
             suffix == QStringLiteral("lnk"))) {
            cacheKey = QStringLiteral("path:%1").arg(path.toLower());
        }
        else {
            cacheKey = QStringLiteral("file:%1").arg(suffix);
        }
    }
    const auto cached = cache.constFind(cacheKey);
    if (cached != cache.cend()) {
        return cached.value();
    }

#ifdef Q_OS_WIN
    if (drive && remote) {
        SHSTOCKICONINFO stockInfo {};
        stockInfo.cbSize = sizeof(stockInfo);
        if (SUCCEEDED(SHGetStockIconInfo(
                    SIID_DRIVEFIXED,
                    SHGSI_ICON | SHGSI_SMALLICON,
                    &stockInfo)) &&
            stockInfo.hIcon != nullptr) {
            QImage image = QImage::fromHICON(stockInfo.hIcon);
            DestroyIcon(stockInfo.hIcon);
            if (!image.isNull()) {
                image = image.scaled(kIconSize,
                                     kIconSize,
                                     Qt::KeepAspectRatio,
                                     Qt::SmoothTransformation);
                cache.insert(cacheKey, image);
                return image;
            }
        }
    }

    QString lookupPath = path;
    if (drive && remote) {
        lookupPath = driveRootName(path) + QStringLiteral("\\");
    }
    else if (remote) {
        lookupPath = name;
    }
    lookupPath = QDir::toNativeSeparators(lookupPath);

    SHFILEINFOW info {};
    DWORD attributes = directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    UINT flags = SHGFI_ICON | SHGFI_SMALLICON;
    if (remote) {
        flags |= SHGFI_USEFILEATTRIBUTES;
    }
    const DWORD_PTR result = SHGetFileInfoW(
            reinterpret_cast<LPCWSTR>(lookupPath.utf16()),
            attributes,
            &info,
            sizeof(info),
            flags);
    if (result != 0 && info.hIcon != nullptr) {
        QImage image = QImage::fromHICON(info.hIcon);
        DestroyIcon(info.hIcon);
        if (!image.isNull()) {
            image = image.scaled(kIconSize,
                                 kIconSize,
                                 Qt::KeepAspectRatio,
                                 Qt::SmoothTransformation);
            cache.insert(cacheKey, image);
            return image;
        }
    }
#else
    Q_UNUSED(path);
    Q_UNUSED(name);
    Q_UNUSED(remote);
#endif

    QImage fallback(kIconSize, kIconSize, QImage::Format_ARGB32_Premultiplied);
    fallback.fill(Qt::transparent);
    QPainter painter(&fallback);
    painter.setRenderHint(QPainter::Antialiasing);
    if (directory || drive) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(drive ? QColor(95, 176, 255) : QColor(255, 194, 71));
        painter.drawRoundedRect(QRectF(1, 6, 20, 14), 2, 2);
        painter.drawRoundedRect(QRectF(3, 3, 9, 6), 2, 2);
    }
    else {
        QPainterPath page;
        page.moveTo(4, 1);
        page.lineTo(14, 1);
        page.lineTo(20, 7);
        page.lineTo(20, 21);
        page.lineTo(4, 21);
        page.closeSubpath();
        painter.setPen(QPen(QColor(145, 156, 170), 1));
        painter.setBrush(QColor(239, 243, 247));
        painter.drawPath(page);
    }
    cache.insert(cacheKey, fallback);
    return fallback;
}

QString remoteJoin(const QString& parent, const QString& name)
{
    return parent.isEmpty() ? name : parent + QLatin1Char('/') + name;
}

QString remoteParent(QString path)
{
    while (path.endsWith(QLatin1Char('/'))) {
        path.chop(1);
    }
    const int slash = path.lastIndexOf(QLatin1Char('/'));
    return slash < 0 ? QString() : path.left(slash);
}

QString displaySize(quint64 bytes)
{
    if (bytes >= 1024ULL * 1024ULL * 1024ULL) {
        return QStringLiteral("%1 GB").arg(static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0), 0, 'f', 1);
    }
    if (bytes >= 1024ULL * 1024ULL) {
        return QStringLiteral("%1 MB").arg(static_cast<double>(bytes) / (1024.0 * 1024.0), 0, 'f', 1);
    }
    if (bytes >= 1024ULL) {
        return QStringLiteral("%1 KB").arg(static_cast<double>(bytes) / 1024.0, 0, 'f', 1);
    }
    return QStringLiteral("%1 B").arg(bytes);
}

QString errorMessage(const FileMapping::Error& error)
{
    if (error.message.contains(QStringLiteral("closed"), Qt::CaseInsensitive)) {
        return QCoreApplication::translate(
                "FileTransferWindow",
                "The host closed the file transfer connection.");
    }

    switch (error.kind) {
    case FileMapping::ErrorKind::None:
        return error.message.isEmpty()
                ? QCoreApplication::translate(
                          "FileTransferWindow",
                          "Unknown file transfer error.")
                : error.message;
    case FileMapping::ErrorKind::Unavailable:
        return QCoreApplication::translate(
                "FileTransferWindow",
                "The host file transfer service is unavailable.");
    case FileMapping::ErrorKind::Unauthorized:
        return QCoreApplication::translate(
                "FileTransferWindow",
                "The host denied file transfer authorization.");
    case FileMapping::ErrorKind::Timeout:
        return QCoreApplication::translate(
                "FileTransferWindow",
                "The connection to the host timed out.");
    case FileMapping::ErrorKind::Network:
        return QCoreApplication::translate(
                "FileTransferWindow",
                "The file transfer network connection failed.");
    case FileMapping::ErrorKind::NotFound:
        return QCoreApplication::translate(
                "FileTransferWindow",
                "The remote file or folder was not found.");
    case FileMapping::ErrorKind::ReadOnly:
        return QCoreApplication::translate(
                "FileTransferWindow",
                "The remote drive is read-only and cannot accept uploads.");
    case FileMapping::ErrorKind::Conflict:
        return QCoreApplication::translate(
                "FileTransferWindow",
                "An item with the same name already exists.");
    case FileMapping::ErrorKind::Cancelled:
        return QCoreApplication::translate(
                "FileTransferWindow",
                "The transfer was cancelled.");
    case FileMapping::ErrorKind::Unsupported:
        return QCoreApplication::translate(
                "FileTransferWindow",
                "The host does not support this file operation.");
    case FileMapping::ErrorKind::Internal:
        return error.message.isEmpty()
                ? QCoreApplication::translate(
                          "FileTransferWindow",
                          "An internal file transfer error occurred.")
                : QCoreApplication::translate(
                          "FileTransferWindow",
                          "File transfer failed: %1")
                          .arg(error.message);
    }

    return QCoreApplication::translate(
            "FileTransferWindow",
            "Unknown file transfer error.");
}

bool connectionError(const FileMapping::Error& error)
{
    return error.kind == FileMapping::ErrorKind::Network ||
           error.kind == FileMapping::ErrorKind::Timeout ||
           error.kind == FileMapping::ErrorKind::Unauthorized ||
           error.kind == FileMapping::ErrorKind::Unavailable;
}
} // namespace

FileTransferWorker::FileTransferWorker(NvComputer computer)
    : m_Computer(std::move(computer))
{
}

FileTransferWorker::~FileTransferWorker() = default;

void FileTransferWorker::requestCancel()
{
    m_Cancelled.store(true);
}

bool FileTransferWorker::ensureConnected(QString& error)
{
    if (m_Client) {
        return true;
    }

    m_Cancelled.store(false);
    auto client = std::make_unique<FileMappingProtocolAdapter>(m_Computer);
    const FileMapping::Capability capability = client->fetchCapability(kTransferTimeoutMs);
    if (!capability.error.ok()) {
        error = errorMessage(capability.error);
        return false;
    }
    if (!capability.enabled || !capability.listening || capability.sessionToken.isEmpty()) {
        error = tr("Host file transfer is not ready yet. Keep the streaming session connected and try again.");
        return false;
    }

    const FileMapping::Error connectError = client->connectSession(capability, kTransferTimeoutMs);
    if (!connectError.ok()) {
        error = errorMessage(connectError);
        return false;
    }
    m_Client = std::move(client);
    return true;
}

void FileTransferWorker::initialize()
{
    QString error;
    if (!ensureConnected(error)) {
        emit remoteReady({}, error);
        return;
    }

    QVariantList mappings;
    for (const FileMapping::RemoteMapping& mapping : m_Client->mappings()) {
        QVariantMap item;
        item.insert(QStringLiteral("id"), mapping.id);
        item.insert(QStringLiteral("name"), mapping.displayName);
        item.insert(QStringLiteral("volumeLabel"), mapping.volumeLabel);
        item.insert(QStringLiteral("writable"),
                    mapping.mode == QStringLiteral("readwrite") &&
                    mapping.capabilities.contains(QStringLiteral("write")));
        item.insert(QStringLiteral("deletable"),
                    mapping.capabilities.contains(QStringLiteral("delete")));
        mappings.append(item);
    }
    emit remoteReady(mappings, QString());
}

void FileTransferWorker::browseRemote(const QString& mappingId, const QString& path)
{
    QString error;
    if (!ensureConnected(error)) {
        emit remoteListed(mappingId, path, {}, error);
        return;
    }

    const FileMapping::ListResult result = m_Client->list(mappingId, path, kTransferTimeoutMs);
    if (!result.ok()) {
        if (connectionError(result.error)) {
            m_Client.reset();
        }
        emit remoteListed(mappingId, path, {}, errorMessage(result.error));
        return;
    }

    QVariantList entries;
    for (const FileMapping::RemoteEntry& entry : result.entries) {
        QVariantMap item;
        item.insert(QStringLiteral("name"), entry.displayName);
        item.insert(QStringLiteral("path"), entry.path);
        item.insert(QStringLiteral("directory"), entry.directory);
        item.insert(QStringLiteral("size"), QVariant::fromValue(entry.size));
        entries.append(item);
    }
    emit remoteListed(mappingId, path, entries, QString());
}

bool FileTransferWorker::uploadFile(const QString& localPath,
                                    const QString& mappingId,
                                    const QString& remotePath,
                                    int conflictPolicy,
                                    QString& error)
{
    QFile file(localPath);
    if (!file.open(QIODevice::ReadOnly)) {
        error = tr("Could not open the local file: %1").arg(localPath);
        return false;
    }

    const quint64 total = static_cast<quint64>(file.size());
    const QString uploadId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QString uploadPath = remotePath;
    quint64 offset = 0;
    bool first = true;

    do {
        if (m_Cancelled.load()) {
            error = tr("The transfer was cancelled.");
            return false;
        }

        const QByteArray chunk = file.read(kTransferChunkBytes);
        if (chunk.isNull()) {
            error = tr("Could not read the local file: %1").arg(localPath);
            return false;
        }
        const bool complete = offset + static_cast<quint64>(chunk.size()) >= total;
        const FileMapping::WriteResult result = m_Client->write(mappingId,
                                                               uploadPath,
                                                               uploadId,
                                                               offset,
                                                               total,
                                                               chunk,
                                                               first,
                                                               complete,
                                                               conflictPolicyFromValue(conflictPolicy),
                                                               kTransferTimeoutMs);
        if (!result.ok()) {
            if (connectionError(result.error)) {
                m_Client.reset();
            }
            error = errorMessage(result.error);
            return false;
        }

        offset += static_cast<quint64>(chunk.size());
        // Keep all following chunks attached to the actual "(n)" destination
        // selected atomically by Sunshine.
        uploadPath = result.actualPath;
        first = false;
        emit transferProgress(
                tr("Uploading %1").arg(QFileInfo(localPath).fileName()),
                offset,
                total);
    } while (first || offset < total);

    return true;
}

bool FileTransferWorker::uploadItem(const QString& localPath,
                                    const QString& mappingId,
                                    const QString& remotePath,
                                    int conflictPolicy,
                                    QString& error)
{
    if (m_Cancelled.load()) {
        error = tr("The transfer was cancelled.");
        return false;
    }

    const QFileInfo info(localPath);
    if (info.isSymLink()) {
        error = tr("Symbolic links are not supported: %1").arg(localPath);
        return false;
    }
    if (info.isFile()) {
        return uploadFile(
                localPath, mappingId, remotePath, conflictPolicy, error);
    }
    if (!info.isDir()) {
        error = tr("Unsupported local item: %1").arg(localPath);
        return false;
    }

    const FileMapping::PathResult mkdirResult = m_Client->mkdir(
            mappingId,
            remotePath,
            conflictPolicyFromValue(conflictPolicy),
            kTransferTimeoutMs);
    if (!mkdirResult.ok()) {
        if (connectionError(mkdirResult.error)) {
            m_Client.reset();
        }
        error = errorMessage(mkdirResult.error);
        return false;
    }

    const QFileInfoList children = QDir(localPath).entryInfoList(
            QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
            QDir::DirsFirst | QDir::Name | QDir::IgnoreCase);
    for (const QFileInfo& child : children) {
        if (!uploadItem(child.absoluteFilePath(),
                        mappingId,
                        remoteJoin(mkdirResult.actualPath, child.fileName()),
                        conflictPolicy,
                        error)) {
            return false;
        }
    }
    return true;
}

void FileTransferWorker::upload(const QString& localPath,
                                const QString& mappingId,
                                const QString& remoteDirectory,
                                int conflictPolicy)
{
    QString error;
    if (!ensureConnected(error)) {
        emit transferFinished(false, error);
        return;
    }

    m_Cancelled.store(false);
    const QFileInfo source(localPath);
    if (!source.exists()) {
        emit transferFinished(
                false,
                tr("The selected local file or folder no longer exists."));
        return;
    }

    const QString remotePath = remoteJoin(remoteDirectory, source.fileName());
    // Collision handling lives on Sunshine so KeepBoth remains race-free when
    // multiple clients upload the same name at once.
    if (uploadItem(
                localPath, mappingId, remotePath, conflictPolicy, error)) {
        emit transferFinished(
                true,
                tr("Uploaded \"%1\" to the host.").arg(source.fileName()));
    }
    else {
        emit transferFinished(false, error);
    }
}

bool FileTransferWorker::downloadFile(const QString& mappingId,
                                      const QString& remotePath,
                                      const QString& localPath,
                                      QString& error)
{
    const FileMapping::StatResult stat = m_Client->stat(mappingId, remotePath, kTransferTimeoutMs);
    if (!stat.ok()) {
        if (connectionError(stat.error)) {
            m_Client.reset();
        }
        error = errorMessage(stat.error);
        return false;
    }
    if (!stat.stat.exists || stat.stat.directory) {
        error = tr("The remote file is no longer available: %1").arg(remotePath);
        return false;
    }

    const QFileInfo targetInfo(localPath);
    QDir().mkpath(targetInfo.absolutePath());
    QSaveFile target(localPath);
    if (!target.open(QIODevice::WriteOnly)) {
        error = tr("Could not create a temporary download file in %1.")
                        .arg(targetInfo.absolutePath());
        return false;
    }

    quint64 offset = 0;
    const quint64 total = stat.stat.size;
    while (offset < total) {
        if (m_Cancelled.load()) {
            error = tr("The transfer was cancelled.");
            return false;
        }

        const quint32 length = static_cast<quint32>(
                std::min<quint64>(kTransferChunkBytes, total - offset));
        const FileMapping::ReadResult result = m_Client->read(mappingId,
                                                             remotePath,
                                                             offset,
                                                             length,
                                                             kTransferTimeoutMs);
        if (!result.ok()) {
            if (connectionError(result.error)) {
                m_Client.reset();
            }
            error = errorMessage(result.error);
            return false;
        }
        if (result.data.isEmpty()) {
            error = tr("The host returned incomplete file data.");
            return false;
        }
        if (target.write(result.data) != result.data.size()) {
            error = tr("Could not write the local file: %1").arg(localPath);
            return false;
        }
        offset += static_cast<quint64>(result.data.size());
        emit transferProgress(
                tr("Downloading %1").arg(QFileInfo(remotePath).fileName()),
                offset,
                total);
    }

    // QSaveFile commits by atomically replacing an existing regular file. This
    // gives explicit Overwrite transfers the same transactional guarantee as
    // Sunshine uploads and still handles Chinese paths on Windows.
    if (!target.commit()) {
        const QString renameError = target.errorString();
        error = tr("Could not save the downloaded file: %1 (%2)")
                        .arg(localPath, renameError);
        return false;
    }
    return true;
}

bool FileTransferWorker::downloadItem(const QString& mappingId,
                                      const QString& remotePath,
                                      bool directory,
                                      const QString& localPath,
                                      QString& error)
{
    if (m_Cancelled.load()) {
        error = tr("The transfer was cancelled.");
        return false;
    }
    if (!directory) {
        return downloadFile(mappingId, remotePath, localPath, error);
    }

    if (!QDir().mkpath(localPath)) {
        error = tr("Could not create the local folder: %1").arg(localPath);
        return false;
    }

    const FileMapping::ListResult result = m_Client->list(mappingId, remotePath, kTransferTimeoutMs);
    if (!result.ok()) {
        if (connectionError(result.error)) {
            m_Client.reset();
        }
        error = errorMessage(result.error);
        return false;
    }
    for (const FileMapping::RemoteEntry& child : result.entries) {
        if (!downloadItem(mappingId,
                          child.path,
                          child.directory,
                          QDir(localPath).filePath(child.displayName),
                          error)) {
            return false;
        }
    }
    return true;
}

void FileTransferWorker::download(const QString& mappingId,
                                  const QString& remotePath,
                                  bool directory,
                                  const QString& localDirectory,
                                  int conflictPolicy)
{
    QString error;
    if (!ensureConnected(error)) {
        emit transferFinished(false, error);
        return;
    }

    m_Cancelled.store(false);
    const QString name = QFileInfo(remotePath).fileName();
    QString localPath = QDir(localDirectory).filePath(name);
    bool destinationExisted = QFileInfo::exists(localPath);
    if (destinationExisted &&
        conflictPolicyFromValue(conflictPolicy) ==
                FileMapping::ConflictPolicy::KeepBoth) {
        localPath = uniqueLocalSiblingPath(localPath);
        if (localPath.isEmpty()) {
            emit transferFinished(
                    false,
                    tr("Could not allocate a unique local name for \"%1\".")
                            .arg(name));
            return;
        }
        // The suffixed destination is new even though the originally
        // requested name existed.
        destinationExisted = false;
    }
    if (QFileInfo::exists(localPath)) {
        const QFileInfo existing(localPath);
        if (existing.isDir() != directory) {
            emit transferFinished(
                    false,
                    tr("Cannot overwrite \"%1\" because the existing item has a different type.")
                            .arg(name));
            return;
        }
    }

    if (downloadItem(mappingId, remotePath, directory, localPath, error)) {
        emit transferFinished(
                true,
                tr("Downloaded \"%1\" to this computer.").arg(name));
    }
    else {
        // Never remove a pre-existing destination after a failed merge. Only a
        // fresh KeepBoth directory created by this transfer is safe to clean.
        if (directory && !destinationExisted) {
            QDir(localPath).removeRecursively();
        }
        emit transferFinished(false, error);
    }
}

void FileTransferWorker::createRemoteFile(const QString& mappingId,
                                          const QString& remotePath,
                                          int conflictPolicy)
{
    QString error;
    if (!ensureConnected(error)) {
        emit operationFinished(false, error);
        return;
    }

    // Empty files use the normal transactional write path, which keeps the
    // protocol surface small and applies the same path validation as uploads.
    const QString uploadId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const FileMapping::WriteResult result = m_Client->write(
            mappingId,
            remotePath,
            uploadId,
            0,
            0,
            QByteArray(),
            true,
            true,
            conflictPolicyFromValue(conflictPolicy),
            kTransferTimeoutMs);
    if (!result.ok()) {
        if (connectionError(result.error)) {
            m_Client.reset();
        }
        emit operationFinished(false, errorMessage(result.error));
        return;
    }
    emit operationFinished(true, tr("Created file \"%1\".")
                                    .arg(QFileInfo(result.actualPath).fileName()));
}

void FileTransferWorker::createRemoteFolder(const QString& mappingId,
                                            const QString& remotePath,
                                            int conflictPolicy)
{
    QString error;
    if (!ensureConnected(error)) {
        emit operationFinished(false, error);
        return;
    }

    const FileMapping::PathResult result = m_Client->mkdir(
            mappingId,
            remotePath,
            conflictPolicyFromValue(conflictPolicy),
            kTransferTimeoutMs);
    if (!result.ok()) {
        if (connectionError(result.error)) {
            m_Client.reset();
        }
        emit operationFinished(false, errorMessage(result.error));
        return;
    }
    emit operationFinished(true, tr("Created folder \"%1\".")
                                    .arg(QFileInfo(result.actualPath).fileName()));
}

void FileTransferWorker::renameRemote(const QString& mappingId,
                                      const QString& remotePath,
                                      const QString& destinationPath)
{
    QString error;
    if (!ensureConnected(error)) {
        emit operationFinished(false, error);
        return;
    }

    const FileMapping::PathResult result = m_Client->rename(
            mappingId, remotePath, destinationPath, kTransferTimeoutMs);
    if (!result.ok()) {
        if (connectionError(result.error)) {
            m_Client.reset();
        }
        emit operationFinished(false, errorMessage(result.error));
        return;
    }
    emit operationFinished(true, tr("Renamed item to \"%1\".")
                                    .arg(QFileInfo(destinationPath).fileName()));
}

void FileTransferWorker::deleteRemote(const QString& mappingId,
                                      const QString& remotePath,
                                      bool recursive)
{
    QString error;
    if (!ensureConnected(error)) {
        emit operationFinished(false, error);
        return;
    }

    const FileMapping::Error result = m_Client->remove(
            mappingId, remotePath, recursive, kTransferTimeoutMs);
    if (!result.ok()) {
        if (connectionError(result)) {
            m_Client.reset();
        }
        emit operationFinished(false, errorMessage(result));
        return;
    }
    emit operationFinished(true, tr("Deleted \"%1\".")
                                    .arg(QFileInfo(remotePath).fileName()));
}

FileTransferWindow::FileTransferWindow(NvComputer computer)
{
    setTitle(tr("Moonlight File Transfer"));
    setFlags(Qt::Window | Qt::WindowTitleHint | Qt::WindowSystemMenuHint |
             Qt::WindowStaysOnTopHint |
             Qt::WindowMinMaxButtonsHint | Qt::WindowCloseButtonHint);
    resize(1080, 680);
    setMinimumSize(QSize(820, 520));

    if (QScreen* screen = QGuiApplication::primaryScreen()) {
        const QRect area = screen->availableGeometry();
        setPosition(area.center() - QPoint(width() / 2, height() / 2));
    }

    loadLocalRoot();
    setStatus(tr("Connecting to host drives..."));

    m_Worker = new FileTransferWorker(std::move(computer));
    m_Worker->moveToThread(&m_WorkerThread);
    connect(&m_WorkerThread, &QThread::finished, m_Worker, &QObject::deleteLater);
    connect(this, &FileTransferWindow::initializeWorker,
            m_Worker, &FileTransferWorker::initialize);
    connect(this, &FileTransferWindow::requestRemoteList,
            m_Worker, &FileTransferWorker::browseRemote);
    connect(this, &FileTransferWindow::requestUpload,
            m_Worker, &FileTransferWorker::upload);
    connect(this, &FileTransferWindow::requestDownload,
            m_Worker, &FileTransferWorker::download);
    connect(this, &FileTransferWindow::requestCreateRemoteFile,
            m_Worker, &FileTransferWorker::createRemoteFile);
    connect(this, &FileTransferWindow::requestCreateRemoteFolder,
            m_Worker, &FileTransferWorker::createRemoteFolder);
    connect(this, &FileTransferWindow::requestRenameRemote,
            m_Worker, &FileTransferWorker::renameRemote);
    connect(this, &FileTransferWindow::requestDeleteRemote,
            m_Worker, &FileTransferWorker::deleteRemote);
    connect(m_Worker, &FileTransferWorker::remoteReady,
            this, &FileTransferWindow::onRemoteReady);
    connect(m_Worker, &FileTransferWorker::remoteListed,
            this, &FileTransferWindow::onRemoteListed);
    connect(m_Worker, &FileTransferWorker::transferProgress,
            this, &FileTransferWindow::onTransferProgress);
    connect(m_Worker, &FileTransferWorker::transferFinished,
            this, &FileTransferWindow::onTransferFinished);
    connect(m_Worker, &FileTransferWorker::operationFinished,
            this, &FileTransferWindow::onOperationFinished);
    m_WorkerThread.start();
    emit initializeWorker();
}

FileTransferWindow::~FileTransferWindow()
{
    if (m_Worker) {
        m_Worker->requestCancel();
    }
    m_WorkerThread.quit();
    m_WorkerThread.wait(15000);
}

void FileTransferWindow::showAndActivate()
{
    show();
    raise();
    requestActivate();
}

void FileTransferWindow::queueExternalUpload(const QString& localPath)
{
    const QFileInfo source(localPath);
    if (!source.exists() || source.isRoot()) {
        setStatus(tr("The dropped file or folder is no longer available."), true);
        return;
    }

    m_ExternalUploadQueue.append(source.absoluteFilePath());
    showAndActivate();
    startNextExternalUpload();
}

QRect FileTransferWindow::localPaneRect() const
{
    const int paneWidth = (width() - kMargin * 2 - kCenterWidth) / 2;
    return QRect(kMargin, kRowsTop, paneWidth, height() - kRowsTop - kStatusHeight);
}

QRect FileTransferWindow::remotePaneRect() const
{
    const QRect left = localPaneRect();
    return QRect(left.right() + 1 + kCenterWidth,
                 kRowsTop,
                 left.width(),
                 left.height());
}

QRect FileTransferWindow::localPathRect() const
{
    const QRect pane = localPaneRect();
    return QRect(pane.x(), kHeaderHeight, pane.width(), kPathHeight);
}

QRect FileTransferWindow::remotePathRect() const
{
    const QRect pane = remotePaneRect();
    return QRect(pane.x(), kHeaderHeight, pane.width(), kPathHeight);
}

QRect FileTransferWindow::uploadButtonRect() const
{
    const QRect left = localPaneRect();
    return QRect(left.right() + 13, 205, kCenterWidth - 26, 42);
}

QRect FileTransferWindow::downloadButtonRect() const
{
    const QRect left = localPaneRect();
    return QRect(left.right() + 13, 265, kCenterWidth - 26, 42);
}

QRect FileTransferWindow::refreshButtonRect() const
{
    const QRect left = localPaneRect();
    return QRect(left.right() + 13, 345, kCenterWidth - 26, 38);
}

QRect FileTransferWindow::conflictButtonRect() const
{
    const QRect left = localPaneRect();
    return QRect(left.right() + 8, 88, kCenterWidth - 16, 52);
}

QRect FileTransferWindow::receiveDirectoryButtonRect() const
{
    const QRect left = localPaneRect();
    return QRect(left.right() + 8, 405, kCenterWidth - 16, 48);
}

QRect FileTransferWindow::actionButtonRect(bool local, int actionIndex) const
{
    const QRect pane = local ? localPaneRect() : remotePaneRect();
    constexpr int gap = 5;
    const int width = (pane.width() - gap * (kActionCount - 1)) / kActionCount;
    return QRect(pane.x() + actionIndex * (width + gap), 88, width, 30);
}

QRect FileTransferWindow::dialogRect() const
{
    const int dialogWidth = std::min(540, width() - 80);
    return QRect((width() - dialogWidth) / 2,
                 (height() - 210) / 2,
                 dialogWidth,
                 210);
}

QRect FileTransferWindow::dialogOkRect() const
{
    const QRect dialog = dialogRect();
    return QRect(dialog.right() - 208, dialog.bottom() - 52, 88, 34);
}

QRect FileTransferWindow::dialogCancelRect() const
{
    const QRect dialog = dialogRect();
    return QRect(dialog.right() - 104, dialog.bottom() - 52, 88, 34);
}

int FileTransferWindow::visibleRowCount() const
{
    return std::max(
            1,
            (localPaneRect().height() - kColumnHeaderHeight) / kRowHeight);
}

QRect FileTransferWindow::rowRect(bool local, int visibleRow) const
{
    const QRect pane = local ? localPaneRect() : remotePaneRect();
    return QRect(pane.x() + 1,
                 pane.y() + kColumnHeaderHeight +
                         visibleRow * kRowHeight + 1,
                 pane.width() - 2,
                 kRowHeight);
}

int FileTransferWindow::rowAt(bool local, const QPoint& point) const
{
    const QRect pane = local ? localPaneRect() : remotePaneRect();
    if (!pane.contains(point)) {
        return -1;
    }
    if (point.y() < pane.y() + kColumnHeaderHeight) {
        return -1;
    }
    const int visible =
            (point.y() - pane.y() - kColumnHeaderHeight) / kRowHeight;
    const int offset = local ? m_LocalScroll : m_RemoteScroll;
    const int index = offset + visible;
    const int count = local ? m_LocalEntries.size() : m_RemoteEntries.size();
    return index >= 0 && index < count ? index : -1;
}

void FileTransferWindow::loadLocalRoot()
{
    m_LocalPath.clear();
    m_LocalEntries.clear();
    for (const QFileInfo& drive : QDir::drives()) {
        Entry entry;
        entry.path = drive.absoluteFilePath();
        entry.name = localDriveDisplayName(entry.path);
        entry.directory = true;
        entry.drive = true;
        entry.writable = drive.isWritable();
        entry.icon = fileIcon(
                entry.path, entry.name, entry.directory, entry.drive, false);
        m_LocalEntries.append(std::move(entry));
    }
    m_LocalSelection = -1;
    m_LocalScroll = 0;
    update();
}

void FileTransferWindow::browseLocal(const QString& path)
{
    QDir directory(path);
    if (!directory.exists()) {
        setStatus(tr("The local folder is unavailable: %1").arg(path), true);
        return;
    }

    QVector<Entry> entries;
    const QFileInfoList infos = directory.entryInfoList(
            QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
            QDir::DirsFirst | QDir::Name | QDir::IgnoreCase);
    entries.reserve(infos.size());
    for (const QFileInfo& info : infos) {
        Entry entry;
        entry.name = info.fileName();
        entry.path = info.absoluteFilePath();
        entry.directory = info.isDir();
        entry.size = info.isFile() ? static_cast<quint64>(info.size()) : 0;
        entry.icon = fileIcon(
                entry.path, entry.name, entry.directory, false, false);
        entries.append(std::move(entry));
    }

    m_LocalPath = directory.absolutePath();
    m_LocalEntries = std::move(entries);
    m_LocalSelection = -1;
    m_LocalScroll = 0;
    update();
}

void FileTransferWindow::refreshLocal()
{
    if (m_LocalPath.isEmpty()) {
        loadLocalRoot();
    }
    else {
        browseLocal(m_LocalPath);
    }
}

void FileTransferWindow::refreshRemote()
{
    if (m_RemoteMappingId.isEmpty()) {
        if (m_RemoteRoots.isEmpty()) {
            m_Busy = true;
            setStatus(tr("Reconnecting to host drives..."));
            emit initializeWorker();
            return;
        }
        m_RemoteEntries = m_RemoteRoots;
        m_RemoteSelection = -1;
        m_RemoteScroll = 0;
        update();
        return;
    }

    m_Busy = true;
    setStatus(tr("Loading remote folder..."));
    emit requestRemoteList(m_RemoteMappingId, m_RemotePath);
}

void FileTransferWindow::openLocalSelection()
{
    if (m_LocalSelection < 0 || m_LocalSelection >= m_LocalEntries.size()) {
        return;
    }
    const Entry& entry = m_LocalEntries.at(m_LocalSelection);
    if (entry.directory) {
        browseLocal(entry.path);
    }
}

void FileTransferWindow::openRemoteSelection()
{
    if (m_RemoteSelection < 0 || m_RemoteSelection >= m_RemoteEntries.size()) {
        return;
    }
    const Entry entry = m_RemoteEntries.at(m_RemoteSelection);
    if (!entry.directory) {
        return;
    }
    if (entry.drive) {
        m_RemoteMappingId = entry.mappingId;
        m_RemoteMappingName = entry.name;
        m_RemotePath.clear();
        m_RemoteWritable = entry.writable;
        m_RemoteDeleteAllowed = entry.deletable;
    }
    else {
        m_RemotePath = entry.path;
    }
    refreshRemote();
}

void FileTransferWindow::localUp()
{
    if (m_LocalPath.isEmpty()) {
        return;
    }
    QDir directory(m_LocalPath);
    if (directory.isRoot() || !directory.cdUp()) {
        loadLocalRoot();
    }
    else {
        browseLocal(directory.absolutePath());
    }
}

void FileTransferWindow::remoteUp()
{
    if (m_RemoteMappingId.isEmpty()) {
        return;
    }
    if (m_RemotePath.isEmpty()) {
        m_RemoteMappingId.clear();
        m_RemoteMappingName.clear();
        m_RemoteWritable = false;
        m_RemoteDeleteAllowed = false;
    }
    else {
        m_RemotePath = remoteParent(m_RemotePath);
    }
    refreshRemote();
}

void FileTransferWindow::beginUpload()
{
    if (m_Busy) {
        return;
    }
    if (m_LocalSelection < 0 || m_LocalSelection >= m_LocalEntries.size()) {
        setStatus(tr("Select a local file or folder first."), true);
        return;
    }
    if (m_RemoteMappingId.isEmpty()) {
        setStatus(tr("Open a remote drive and enter the destination folder first."), true);
        return;
    }
    if (!m_RemoteWritable) {
        setStatus(tr("This remote drive is read-only."), true);
        return;
    }

    const Entry entry = m_LocalEntries.at(m_LocalSelection);
    if (entry.drive) {
        setStatus(
                tr("Open a local drive and select a file or folder. Entire drives cannot be transferred."),
                true);
        return;
    }

    m_Busy = true;
    m_ProgressVisible = true;
    m_ProgressPercent = 0;
    setStatus(tr("Preparing to upload: %1").arg(entry.name));
    emit requestUpload(
            entry.path,
            m_RemoteMappingId,
            m_RemotePath,
            conflictPolicyValue());
}

void FileTransferWindow::beginDownload()
{
    if (m_Busy) {
        return;
    }
    if (m_RemoteSelection < 0 || m_RemoteSelection >= m_RemoteEntries.size()) {
        setStatus(tr("Select a remote file or folder first."), true);
        return;
    }
    if (m_LocalPath.isEmpty()) {
        setStatus(tr("Open a local drive and enter the destination folder first."), true);
        return;
    }

    const Entry entry = m_RemoteEntries.at(m_RemoteSelection);
    if (entry.drive) {
        setStatus(
                tr("Open a remote drive and select a file or folder. Entire drives cannot be transferred."),
                true);
        return;
    }

    m_Busy = true;
    m_ProgressVisible = true;
    m_ProgressPercent = 0;
    setStatus(tr("Preparing to download: %1").arg(entry.name));
    emit requestDownload(
            m_RemoteMappingId,
            entry.path,
            entry.directory,
            m_LocalPath,
            conflictPolicyValue());
}

int FileTransferWindow::conflictPolicyValue() const
{
    return static_cast<int>(
            StreamingPreferences::get()->fileTransferConflictPolicy);
}

void FileTransferWindow::toggleConflictPolicy()
{
    StreamingPreferences* preferences = StreamingPreferences::get();
    preferences->fileTransferConflictPolicy =
            preferences->fileTransferConflictPolicy ==
                    StreamingPreferences::FTCP_KEEP_BOTH
            ? StreamingPreferences::FTCP_OVERWRITE
            : StreamingPreferences::FTCP_KEEP_BOTH;
    preferences->save();
    setStatus(preferences->fileTransferConflictPolicy ==
                      StreamingPreferences::FTCP_KEEP_BOTH
              ? tr("Name conflicts will keep both items by adding (1), (2), ...")
              : tr("Name conflicts will overwrite existing files."));
}

void FileTransferWindow::beginFileOperation(bool local, int actionIndex)
{
    if (m_Busy || m_DialogVisible) {
        return;
    }
    if ((local && m_LocalPath.isEmpty()) ||
        (!local && m_RemoteMappingId.isEmpty())) {
        setStatus(local
                          ? tr("Open a local drive before managing files.")
                          : tr("Open a remote drive before managing files."),
                  true);
        return;
    }
    if (!local && !m_RemoteWritable) {
        setStatus(tr("This remote drive is read-only."), true);
        return;
    }

    if (actionIndex == 0 || actionIndex == 1) {
        showNameDialog(local, actionIndex);
        return;
    }

    const int selection = local ? m_LocalSelection : m_RemoteSelection;
    const QVector<Entry>& entries = local ? m_LocalEntries : m_RemoteEntries;
    if (selection < 0 || selection >= entries.size() ||
        entries.at(selection).drive) {
        setStatus(tr("Select a file or folder first."), true);
        return;
    }
    if (!local && actionIndex == 3 && !m_RemoteDeleteAllowed) {
        setStatus(tr("This host does not allow remote deletion."), true);
        return;
    }

    if (actionIndex == 2) {
        showNameDialog(local, actionIndex, entries.at(selection).name);
    }
    else if (actionIndex == 3) {
        showDeleteDialog(local);
    }
}

void FileTransferWindow::showNameDialog(bool local,
                                        int actionIndex,
                                        const QString& initialText)
{
    m_DialogVisible = true;
    m_DialogConfirmOnly = false;
    m_DialogLocal = local;
    m_DialogAction = actionIndex;
    m_DialogText = initialText;
    m_DialogPreedit.clear();
    m_DialogTitle = actionIndex == 0
            ? tr("New folder name")
            : actionIndex == 1
            ? tr("New file name")
            : tr("Rename item");
    QGuiApplication::inputMethod()->show();
    update();
}

void FileTransferWindow::showDeleteDialog(bool local)
{
    const int selection = local ? m_LocalSelection : m_RemoteSelection;
    const QVector<Entry>& entries = local ? m_LocalEntries : m_RemoteEntries;
    if (selection < 0 || selection >= entries.size()) {
        return;
    }
    m_DialogVisible = true;
    m_DialogConfirmOnly = true;
    m_DialogLocal = local;
    m_DialogAction = 3;
    m_DialogText = entries.at(selection).name;
    m_DialogPreedit.clear();
    m_DialogTitle = tr("Permanently delete this item?");
    update();
}

void FileTransferWindow::closeDialog()
{
    m_DialogVisible = false;
    m_DialogConfirmOnly = false;
    m_DialogAction = -1;
    m_DialogTitle.clear();
    m_DialogText.clear();
    m_DialogPreedit.clear();
    QGuiApplication::inputMethod()->hide();
    update();
}

void FileTransferWindow::acceptDialog()
{
    if (!m_DialogVisible) {
        return;
    }

    const bool local = m_DialogLocal;
    const int action = m_DialogAction;
    const QString name = m_DialogText.trimmed();
    const int selection = local ? m_LocalSelection : m_RemoteSelection;
    const QVector<Entry>& entries = local ? m_LocalEntries : m_RemoteEntries;
    Entry selected;
    if (selection >= 0 && selection < entries.size()) {
        selected = entries.at(selection);
    }

    if (!m_DialogConfirmOnly &&
        (name.isEmpty() || name == QStringLiteral(".") ||
         name == QStringLiteral("..") || name.contains('/') ||
         name.contains('\\'))) {
        setStatus(tr("Enter a single valid file or folder name."), true);
        return;
    }
    closeDialog();

    if (local) {
        bool ok = false;
        QString operationName = name;
        const bool keepBoth =
                conflictPolicyFromValue(conflictPolicyValue()) ==
                FileMapping::ConflictPolicy::KeepBoth;
        QString destinationPath = QDir(m_LocalPath).filePath(name);

        // Local file-manager operations follow the same visible conflict
        // policy as transfers. KeepBoth finds an Explorer-style suffix;
        // Overwrite is applied only after the user selected that policy.
        if (action != 3 && action != 2 && QFileInfo::exists(destinationPath) &&
            keepBoth) {
            destinationPath = uniqueLocalSiblingPath(destinationPath);
            operationName = QFileInfo(destinationPath).fileName();
        }
        if (action == 0) {
            const QFileInfo destination(destinationPath);
            ok = destination.isDir() || QDir().mkdir(destinationPath);
        }
        else if (action == 1) {
            QFile file(destinationPath);
            const QIODevice::OpenMode mode = keepBoth
                    ? QIODevice::WriteOnly | QIODevice::NewOnly
                    : QIODevice::WriteOnly | QIODevice::Truncate;
            ok = !QFileInfo(destinationPath).isDir() && file.open(mode);
        }
        else if (action == 2) {
            destinationPath = QDir(m_LocalPath).filePath(name);
            if (QDir::cleanPath(destinationPath) ==
                QDir::cleanPath(selected.path)) {
                ok = true;
            }
            else {
                // Rename collisions keep the destination intact. Destructive
                // replacement remains limited to explicit transfer/create
                // actions where the Overwrite policy is unambiguous.
                ok = !QFileInfo::exists(destinationPath) &&
                     QDir(m_LocalPath).rename(selected.name, name);
            }
        }
        else if (action == 3) {
            operationName = selected.name;
            ok = selected.directory
                    ? QDir(selected.path).removeRecursively()
                    : QFile::remove(selected.path);
        }
        setStatus(ok
                          ? tr("Local file operation completed: %1")
                                    .arg(operationName)
                          : tr("Local file operation failed: %1")
                                    .arg(operationName),
                  !ok);
        refreshLocal();
        return;
    }

    m_Busy = true;
    setStatus(tr("Applying remote file operation..."));
    if (action == 0) {
        emit requestCreateRemoteFolder(
                m_RemoteMappingId,
                remoteJoin(m_RemotePath, name),
                conflictPolicyValue());
    }
    else if (action == 1) {
        emit requestCreateRemoteFile(
                m_RemoteMappingId,
                remoteJoin(m_RemotePath, name),
                conflictPolicyValue());
    }
    else if (action == 2) {
        emit requestRenameRemote(
                m_RemoteMappingId,
                selected.path,
                remoteJoin(remoteParent(selected.path), name));
    }
    else if (action == 3) {
        emit requestDeleteRemote(
                m_RemoteMappingId, selected.path, selected.directory);
    }
}

bool FileTransferWindow::configuredRemoteDestination(
        QString& mappingId,
        QString& remoteDirectory,
        QString& displayPath) const
{
    QString configured =
            StreamingPreferences::get()->fileTransferReceiveDirectory.trimmed();
    if (configured.isEmpty()) {
        return false;
    }
    configured = QDir::fromNativeSeparators(configured);

    if (configured.size() >= 2 && configured.at(1) == QLatin1Char(':') &&
        configured.at(0).isLetter()) {
        const QChar drive = configured.at(0).toLower();
        mappingId = QStringLiteral("drive-") + drive;
        remoteDirectory = configured.mid(2);
        while (remoteDirectory.startsWith('/')) {
            remoteDirectory.remove(0, 1);
        }
        while (remoteDirectory.endsWith('/')) {
            remoteDirectory.chop(1);
        }
        displayPath = drive.toUpper() + QStringLiteral(":\\") +
                      QDir::toNativeSeparators(remoteDirectory);
        return true;
    }

    if (configured.startsWith('/')) {
        mappingId = QStringLiteral("filesystem-root");
        remoteDirectory = configured.mid(1);
        displayPath = configured;
        return true;
    }
    return false;
}

void FileTransferWindow::saveCurrentRemoteReceiveDirectory()
{
    if (m_RemoteMappingId.isEmpty()) {
        setStatus(tr("Open the remote destination folder first."), true);
        return;
    }

    QString destination;
    if (m_RemoteMappingId.startsWith(QStringLiteral("drive-")) &&
        !m_RemoteMappingName.isEmpty()) {
        destination = remoteDriveRootName(
                              m_RemoteMappingId, m_RemoteMappingName) +
                      QStringLiteral("\\") +
                      QDir::toNativeSeparators(m_RemotePath);
    }
    else {
        destination = QStringLiteral("/") + m_RemotePath;
    }
    StreamingPreferences* preferences = StreamingPreferences::get();
    preferences->fileTransferReceiveDirectory = destination;
    preferences->save();
    setStatus(tr("Stream-window drops will be received in %1.")
                      .arg(QDir::toNativeSeparators(destination)));
    startNextExternalUpload();
}

void FileTransferWindow::startNextExternalUpload()
{
    if (m_Busy || m_ExternalUploadActive ||
        m_ExternalUploadQueue.isEmpty()) {
        return;
    }

    QString mappingId;
    QString remoteDirectory;
    QString displayPath;
    if (!configuredRemoteDestination(
                mappingId, remoteDirectory, displayPath)) {
        setStatus(
                tr("Choose a remote folder, then click \"Set receive folder\" before using stream-window drop."),
                true);
        return;
    }

    const auto root = std::find_if(
            m_RemoteRoots.cbegin(),
            m_RemoteRoots.cend(),
            [&mappingId](const Entry& entry) {
                return entry.mappingId == mappingId && entry.writable;
            });
    if (root == m_RemoteRoots.cend()) {
        setStatus(tr("The configured host receive directory is unavailable or read-only: %1")
                          .arg(displayPath),
                  true);
        return;
    }

    const QString sourcePath = m_ExternalUploadQueue.takeFirst();
    const QFileInfo source(sourcePath);
    if (!source.exists()) {
        setStatus(tr("Skipped a dropped item that no longer exists: %1")
                          .arg(sourcePath),
                  true);
        startNextExternalUpload();
        return;
    }

    m_ExternalUploadActive = true;
    m_Busy = true;
    m_ProgressVisible = true;
    m_ProgressPercent = 0;
    setStatus(tr("Uploading dropped item to %1: %2")
                      .arg(displayPath, source.fileName()));
    emit requestUpload(
            source.absoluteFilePath(),
            mappingId,
            remoteDirectory,
            conflictPolicyValue());
}

bool FileTransferWindow::resolveRemoteDropTarget(
        const QPoint& point,
        QString& mappingId,
        QString& remoteDirectory,
        QString& displayPath) const
{
    if (!remotePaneRect().contains(point) &&
        !remotePathRect().contains(point)) {
        return false;
    }

    const int targetIndex = rowAt(false, point);
    if (m_RemoteMappingId.isEmpty()) {
        if (targetIndex < 0 || targetIndex >= m_RemoteEntries.size()) {
            return false;
        }
        const Entry& drive = m_RemoteEntries.at(targetIndex);
        if (!drive.drive || !drive.writable) {
            return false;
        }
        mappingId = drive.mappingId;
        remoteDirectory.clear();
        displayPath = drive.name + QStringLiteral("\\");
        return true;
    }

    if (!m_RemoteWritable) {
        return false;
    }
    mappingId = m_RemoteMappingId;
    remoteDirectory = m_RemotePath;
    if (targetIndex >= 0 && targetIndex < m_RemoteEntries.size()) {
        const Entry& target = m_RemoteEntries.at(targetIndex);
        if (target.directory && !target.drive) {
            remoteDirectory = target.path;
        }
    }

    displayPath = m_RemoteMappingName;
    if (!remoteDirectory.isEmpty()) {
        displayPath += QStringLiteral("\\") +
                       QDir::toNativeSeparators(remoteDirectory);
    }
    else {
        displayPath += QStringLiteral("\\");
    }
    return true;
}

bool FileTransferWindow::resolveLocalDropTarget(
        const QPoint& point,
        QString& localDirectory) const
{
    if (!localPaneRect().contains(point) &&
        !localPathRect().contains(point)) {
        return false;
    }

    localDirectory = m_LocalPath;
    const int targetIndex = rowAt(true, point);
    if (targetIndex >= 0 && targetIndex < m_LocalEntries.size()) {
        const Entry& target = m_LocalEntries.at(targetIndex);
        if (target.directory) {
            localDirectory = target.path;
        }
    }
    return !localDirectory.isEmpty() &&
           QFileInfo(localDirectory).isDir();
}

void FileTransferWindow::updateDrag(const QPoint& point)
{
    m_DragPosition = point;
    m_DragTargetValid = false;
    m_DragHint.clear();

    if (m_DragSourceLocal) {
        QString mappingId;
        QString remoteDirectory;
        QString displayPath;
        m_DragTargetValid = resolveRemoteDropTarget(
                point, mappingId, remoteDirectory, displayPath);
        m_DragHint = m_DragTargetValid
                ? tr("Release to upload to %1").arg(displayPath)
                : tr("Drop onto a remote folder on the right.");
    }
    else {
        QString localDirectory;
        m_DragTargetValid = resolveLocalDropTarget(point, localDirectory);
        m_DragHint = m_DragTargetValid
                ? tr("Release to download to %1")
                          .arg(QDir::toNativeSeparators(localDirectory))
                : tr("Drop onto a local folder on the left.");
    }

    setCursor(m_DragTargetValid
              ? Qt::DragCopyCursor
              : Qt::ForbiddenCursor);
    update();
}

void FileTransferWindow::finishDrag(const QPoint& point)
{
    if (!m_DragActive || m_Busy || m_DragSourceIndex < 0) {
        return;
    }

    if (m_DragSourceLocal) {
        if (m_DragSourceIndex >= m_LocalEntries.size()) {
            return;
        }
        const Entry source = m_LocalEntries.at(m_DragSourceIndex);
        QString mappingId;
        QString remoteDirectory;
        QString displayPath;
        if (!resolveRemoteDropTarget(
                point, mappingId, remoteDirectory, displayPath)) {
            setStatus(
                    tr("Upload cancelled: drop onto a writable remote folder on the right."),
                    true);
            return;
        }
        m_Busy = true;
        m_ProgressVisible = true;
        m_ProgressPercent = 0;
        setStatus(tr("Preparing to upload: %1").arg(source.name));
        emit requestUpload(
                source.path,
                mappingId,
                remoteDirectory,
                conflictPolicyValue());
        return;
    }

    if (m_DragSourceIndex >= m_RemoteEntries.size()) {
        return;
    }
    const Entry source = m_RemoteEntries.at(m_DragSourceIndex);
    QString localDirectory;
    if (!resolveLocalDropTarget(point, localDirectory)) {
        setStatus(
                tr("Download cancelled: drop onto a local folder on the left."),
                true);
        return;
    }
    m_Busy = true;
    m_ProgressVisible = true;
    m_ProgressPercent = 0;
    setStatus(tr("Preparing to download: %1").arg(source.name));
    emit requestDownload(
            m_RemoteMappingId,
            source.path,
            source.directory,
            localDirectory,
            conflictPolicyValue());
}

void FileTransferWindow::resetDrag()
{
    m_DragActive = false;
    m_DragTargetValid = false;
    m_DragSourceIndex = -1;
    m_DragHint.clear();
    unsetCursor();
    update();
}

void FileTransferWindow::clampScrollOffsets()
{
    const int visible = visibleRowCount();
    const int localCount = static_cast<int>(m_LocalEntries.size());
    const int remoteCount = static_cast<int>(m_RemoteEntries.size());
    m_LocalScroll = std::clamp(m_LocalScroll, 0, std::max(0, localCount - visible));
    m_RemoteScroll = std::clamp(m_RemoteScroll, 0, std::max(0, remoteCount - visible));
}

void FileTransferWindow::setStatus(const QString& status, bool error)
{
    m_Status = status;
    m_StatusError = error;
    update();
}

void FileTransferWindow::onRemoteReady(const QVariantList& mappings, const QString& error)
{
    m_Busy = false;
    m_RemoteRoots.clear();
    if (!error.isEmpty()) {
        setStatus(tr("Could not connect to the host drives: %1").arg(error), true);
        return;
    }

    for (const QVariant& value : mappings) {
        const QVariantMap item = value.toMap();
        Entry entry;
        entry.mappingId = item.value(QStringLiteral("id")).toString();
        const QString rootName = remoteDriveRootName(
                entry.mappingId,
                item.value(QStringLiteral("name")).toString());
        entry.name = remoteDriveDisplayName(
                rootName,
                item.value(QStringLiteral("volumeLabel")).toString());
        entry.path.clear();
        entry.directory = true;
        entry.drive = true;
        entry.writable = item.value(QStringLiteral("writable")).toBool();
        // A per-root capability keeps the UI compatible with older Sunshine
        // builds that supported upload but intentionally omitted deletion.
        entry.deletable = item.value(QStringLiteral("deletable")).toBool();
        entry.icon = fileIcon(
                rootName + QStringLiteral("\\"),
                rootName,
                true,
                true,
                true);
        m_RemoteRoots.append(std::move(entry));
    }
    m_RemoteEntries = m_RemoteRoots;
    m_RemoteSelection = -1;
    m_RemoteScroll = 0;
    setStatus(m_RemoteRoots.isEmpty()
              ? tr("The host has no accessible drives.")
              : tr("Connected. Click a button or drag files to the other pane to transfer."),
              m_RemoteRoots.isEmpty());
    startNextExternalUpload();
}

void FileTransferWindow::onRemoteListed(const QString& mappingId,
                                        const QString& path,
                                        const QVariantList& entries,
                                        const QString& error)
{
    if (mappingId != m_RemoteMappingId || path != m_RemotePath) {
        return;
    }

    m_Busy = false;
    if (!error.isEmpty()) {
        setStatus(tr("Could not open the remote folder: %1").arg(error), true);
        return;
    }

    QVector<Entry> converted;
    converted.reserve(entries.size());
    for (const QVariant& value : entries) {
        const QVariantMap item = value.toMap();
        Entry entry;
        entry.name = item.value(QStringLiteral("name")).toString();
        entry.path = item.value(QStringLiteral("path")).toString();
        entry.mappingId = mappingId;
        entry.directory = item.value(QStringLiteral("directory")).toBool();
        entry.size = item.value(QStringLiteral("size")).toULongLong();
        entry.icon = fileIcon(
                entry.path, entry.name, entry.directory, false, true);
        converted.append(std::move(entry));
    }
    m_RemoteEntries = std::move(converted);
    m_RemoteSelection = -1;
    m_RemoteScroll = 0;
    setStatus(tr("Remote folder loaded."));
    startNextExternalUpload();
}

void FileTransferWindow::onTransferProgress(const QString& message, quint64 completed, quint64 total)
{
    if (total == 0) {
        setStatus(message);
    }
    else {
        const int percent = static_cast<int>(std::min<quint64>(100, completed * 100 / total));
        m_ProgressVisible = true;
        m_ProgressPercent = percent;
        setStatus(QStringLiteral("%1 — %2%").arg(message).arg(percent));
    }
}

void FileTransferWindow::onTransferFinished(bool ok, const QString& message)
{
    m_Busy = false;
    m_ProgressVisible = false;
    m_ProgressPercent = 0;
    setStatus(message, !ok);
    if (ok) {
        refreshLocal();
        refreshRemote();
    }
    if (m_ExternalUploadActive) {
        m_ExternalUploadActive = false;
        startNextExternalUpload();
    }
}

void FileTransferWindow::onOperationFinished(bool ok, const QString& message)
{
    m_Busy = false;
    setStatus(message, !ok);
    if (ok) {
        refreshRemote();
    }
}

bool FileTransferWindow::event(QEvent* event)
{
    if (event->type() == QEvent::InputMethod &&
        m_DialogVisible && !m_DialogConfirmOnly) {
        auto* inputEvent = static_cast<QInputMethodEvent*>(event);
        // QKeyEvent::text() is not enough for Chinese IMEs. Commit completed
        // text and paint the current composition until the IME finalizes it.
        m_DialogText += inputEvent->commitString();
        m_DialogPreedit = inputEvent->preeditString();
        inputEvent->accept();
        update();
        return true;
    }
    if (event->type() == QEvent::InputMethodQuery) {
        auto* queryEvent = static_cast<QInputMethodQueryEvent*>(event);
        if (queryEvent->queries().testFlag(Qt::ImEnabled)) {
            queryEvent->setValue(
                    Qt::ImEnabled,
                    m_DialogVisible && !m_DialogConfirmOnly);
        }
        if (queryEvent->queries().testFlag(Qt::ImSurroundingText)) {
            queryEvent->setValue(Qt::ImSurroundingText, m_DialogText);
        }
        if (queryEvent->queries().testFlag(Qt::ImCursorPosition)) {
            queryEvent->setValue(Qt::ImCursorPosition, m_DialogText.size());
        }
        if (queryEvent->queries().testFlag(Qt::ImAnchorPosition)) {
            queryEvent->setValue(Qt::ImAnchorPosition, m_DialogText.size());
        }
        if (queryEvent->queries().testFlag(Qt::ImCursorRectangle)) {
            const QRect dialog = dialogRect();
            queryEvent->setValue(
                    Qt::ImCursorRectangle,
                    QRect(dialog.x() + 34, dialog.y() + 80, 2, 28));
        }
        queryEvent->accept();
        return true;
    }

    if (event->type() == QEvent::DragEnter ||
        event->type() == QEvent::DragMove) {
        auto* dragEvent = static_cast<QDropEvent*>(event);
        QString mappingId;
        QString remoteDirectory;
        QString displayPath;
        const bool hasLocalItem =
                dragEvent->mimeData()->hasUrls() &&
                std::any_of(
                        dragEvent->mimeData()->urls().cbegin(),
                        dragEvent->mimeData()->urls().cend(),
                        [](const QUrl& url) {
                            return url.isLocalFile() &&
                                   QFileInfo::exists(url.toLocalFile());
                        });
        if (!m_Busy &&
            hasLocalItem &&
            resolveRemoteDropTarget(
                    dragEvent->position().toPoint(),
                    mappingId,
                    remoteDirectory,
                    displayPath)) {
            dragEvent->acceptProposedAction();
            return true;
        }
        dragEvent->ignore();
        return true;
    }

    if (event->type() == QEvent::DragLeave) {
        return true;
    }

    if (event->type() == QEvent::Drop) {
        auto* dropEvent = static_cast<QDropEvent*>(event);
        QString localPath;
        for (const QUrl& url : dropEvent->mimeData()->urls()) {
            if (url.isLocalFile() && QFileInfo::exists(url.toLocalFile())) {
                localPath = url.toLocalFile();
                break;
            }
        }

        QString mappingId;
        QString remoteDirectory;
        QString displayPath;
        if (!m_Busy &&
            !localPath.isEmpty() &&
            resolveRemoteDropTarget(
                    dropEvent->position().toPoint(),
                    mappingId,
                    remoteDirectory,
                    displayPath)) {
            const QFileInfo source(localPath);
            if (!source.isRoot()) {
                m_Busy = true;
                m_ProgressVisible = true;
                m_ProgressPercent = 0;
                setStatus(tr("Preparing to upload: %1").arg(source.fileName()));
                emit requestUpload(
                        source.absoluteFilePath(),
                        mappingId,
                        remoteDirectory,
                        conflictPolicyValue());
                dropEvent->acceptProposedAction();
                return true;
            }
        }
        dropEvent->ignore();
        return true;
    }

    return QRasterWindow::event(event);
}

void FileTransferWindow::paintEvent(QPaintEvent*)
{
    clampScrollOffsets();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(QRect(QPoint(0, 0), size()), QColor(18, 21, 26));

    const QColor panel(30, 34, 40);
    const QColor header(25, 29, 35);
    const QColor border(58, 67, 78);
    const QColor text(239, 242, 246);
    const QColor muted(157, 167, 180);
    const QColor accent(50, 133, 255);
    const QColor selected(42, 78, 119);

    painter.setPen(text);
    QFont heading = painter.font();
    heading.setPointSize(12);
    heading.setBold(true);
    painter.setFont(heading);
    painter.drawText(QRect(localPaneRect().x(), 4, localPaneRect().width(), 34),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     tr("This computer"));
    painter.drawText(QRect(remotePaneRect().x(), 4, remotePaneRect().width(), 34),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     tr("Host computer"));

    auto drawPath = [&](const QRect& rect, const QString& path, bool canGoUp) {
        painter.setBrush(panel);
        painter.setPen(border);
        painter.drawRoundedRect(rect, 6, 6);
        painter.setPen(text);
        QFont normal = painter.font();
        normal.setBold(false);
        normal.setPointSize(10);
        painter.setFont(normal);
        const QString shown = path.isEmpty()
                ? tr("Drives")
                : QDir::toNativeSeparators(path);
        painter.drawText(rect.adjusted(12, 0, -82, 0),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         painter.fontMetrics().elidedText(
                                 shown, Qt::ElideMiddle, rect.width() - 100));
        painter.setPen(canGoUp ? accent : muted);
        painter.drawText(QRect(rect.right() - 76, rect.y(), 72, rect.height()),
                         Qt::AlignCenter,
                         tr("↑ Up"));
    };
    drawPath(localPathRect(), m_LocalPath, !m_LocalPath.isEmpty());
    const QString remoteDisplay = m_RemoteMappingId.isEmpty()
            ? QString()
            : m_RemoteMappingName +
                    (m_RemotePath.isEmpty()
                             ? QStringLiteral("\\")
                             : QStringLiteral("\\") +
                                       QDir::toNativeSeparators(m_RemotePath));
    drawPath(remotePathRect(), remoteDisplay, !m_RemoteMappingId.isEmpty());

    const QStringList actionLabels {
        tr("New folder"),
        tr("New file"),
        tr("Rename"),
        tr("Delete")
    };
    auto drawActionBar = [&](bool local) {
        const bool insideRoot = local
                ? !m_LocalPath.isEmpty()
                : !m_RemoteMappingId.isEmpty() && m_RemoteWritable;
        for (int index = 0; index < actionLabels.size(); ++index) {
            const bool enabled =
                    !m_Busy && insideRoot &&
                    (local || index != 3 || m_RemoteDeleteAllowed);
            const QRect rect = actionButtonRect(local, index);
            painter.setBrush(enabled ? QColor(43, 50, 60)
                                     : QColor(33, 38, 45));
            painter.setPen(border);
            painter.drawRoundedRect(rect, 5, 5);
            painter.setPen(enabled ? text : muted);
            QFont actionFont = painter.font();
            actionFont.setBold(false);
            actionFont.setPointSize(9);
            painter.setFont(actionFont);
            painter.drawText(
                    rect.adjusted(4, 0, -4, 0),
                    Qt::AlignCenter,
                    painter.fontMetrics().elidedText(
                            actionLabels.at(index),
                            Qt::ElideRight,
                            rect.width() - 8));
        }
    };
    drawActionBar(true);
    drawActionBar(false);

    painter.setBrush(panel);
    painter.setPen(border);
    painter.drawRoundedRect(localPaneRect(), 6, 6);
    painter.drawRoundedRect(remotePaneRect(), 6, 6);

    auto drawEntries = [&](bool local) {
        const QRect pane = local ? localPaneRect() : remotePaneRect();
        const QRect columnHeader(
                pane.x() + 1,
                pane.y() + 1,
                pane.width() - 2,
                kColumnHeaderHeight - 1);
        painter.fillRect(columnHeader, header);
        painter.setPen(border);
        painter.drawLine(
                columnHeader.bottomLeft(),
                columnHeader.bottomRight());
        QFont columnFont = painter.font();
        columnFont.setBold(false);
        columnFont.setPointSize(9);
        painter.setFont(columnFont);
        painter.setPen(muted);
        painter.drawText(
                columnHeader.adjusted(42, 0, -90, 0),
                Qt::AlignLeft | Qt::AlignVCenter,
                tr("Name"));
        painter.drawText(
                columnHeader.adjusted(
                        columnHeader.width() - 84, 0, -12, 0),
                Qt::AlignRight | Qt::AlignVCenter,
                tr("Size"));

        const QVector<Entry>& entries = local ? m_LocalEntries : m_RemoteEntries;
        const int selection = local ? m_LocalSelection : m_RemoteSelection;
        const int offset = local ? m_LocalScroll : m_RemoteScroll;
        const int visible = visibleRowCount();
        for (int row = 0; row < visible && offset + row < entries.size(); ++row) {
            const int index = offset + row;
            const Entry& entry = entries.at(index);
            const QRect rect = rowRect(local, row);
            if (index == selection) {
                painter.fillRect(rect.adjusted(2, 1, -2, -1), selected);
            }
            else if (row % 2 != 0) {
                painter.fillRect(
                        rect.adjusted(2, 1, -2, -1),
                        QColor(32, 37, 44));
            }

            const QRect iconRect(
                    rect.x() + 10,
                    rect.center().y() - kIconSize / 2,
                    kIconSize,
                    kIconSize);
            if (!entry.icon.isNull()) {
                painter.drawImage(iconRect, entry.icon);
            }

            painter.setPen(index == selection ? Qt::white : text);
            QFont normal = painter.font();
            normal.setBold(entry.drive);
            normal.setPointSize(10);
            painter.setFont(normal);
            const int sizeWidth = 82;
            painter.drawText(rect.adjusted(42, 0, -sizeWidth, 0),
                             Qt::AlignLeft | Qt::AlignVCenter,
                             painter.fontMetrics().elidedText(entry.name,
                                                              Qt::ElideRight,
                                                              rect.width() - sizeWidth - 48));
            if (!entry.directory) {
                painter.setPen(muted);
                painter.drawText(rect.adjusted(rect.width() - sizeWidth, 0, -10, 0),
                                 Qt::AlignRight | Qt::AlignVCenter,
                                 displaySize(entry.size));
            }
        }
    };
    drawEntries(true);
    drawEntries(false);

    auto drawButton = [&](const QRect& rect, const QString& label, bool enabled) {
        painter.setBrush(enabled ? accent : QColor(55, 62, 72));
        painter.setPen(Qt::NoPen);
        painter.drawRoundedRect(rect, 7, 7);
        painter.setPen(enabled ? Qt::white : muted);
        QFont button = painter.font();
        button.setBold(true);
        button.setPointSize(10);
        painter.setFont(button);
        painter.drawText(rect, Qt::AlignCenter, label);
    };
    drawButton(uploadButtonRect(), tr("Upload →"), !m_Busy);
    drawButton(downloadButtonRect(), tr("← Download"), !m_Busy);
    drawButton(refreshButtonRect(), tr("Refresh"), !m_Busy);
    const bool keepBoth =
            StreamingPreferences::get()->fileTransferConflictPolicy ==
            StreamingPreferences::FTCP_KEEP_BOTH;
    drawButton(
            conflictButtonRect(),
            keepBoth
                    ? tr("Conflict:\nKeep both")
                    : tr("Conflict:\nOverwrite"),
            !m_Busy);
    drawButton(
            receiveDirectoryButtonRect(),
            tr("Set receive\nfolder"),
            !m_Busy && !m_RemoteMappingId.isEmpty() && m_RemoteWritable);

    const QRect statusRect(0, height() - kStatusHeight, width(), kStatusHeight);
    painter.fillRect(statusRect, QColor(13, 16, 20));
    if (m_ProgressVisible) {
        const QRect progressTrack(
                kMargin,
                statusRect.y() + 5,
                width() - kMargin * 2,
                4);
        painter.fillRect(progressTrack, QColor(45, 51, 60));
        painter.fillRect(
                QRect(
                        progressTrack.x(),
                        progressTrack.y(),
                        progressTrack.width() * m_ProgressPercent / 100,
                        progressTrack.height()),
                accent);
    }
    painter.setPen(m_StatusError ? QColor(255, 112, 112) : muted);
    QFont statusFont = painter.font();
    statusFont.setBold(false);
    statusFont.setPointSize(10);
    painter.setFont(statusFont);
    painter.drawText(statusRect.adjusted(kMargin, 0, -kMargin, 0),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     painter.fontMetrics().elidedText(m_Status, Qt::ElideRight, width() - kMargin * 2));

    if (m_DragActive) {
        const QRect targetPane = m_DragSourceLocal
                ? remotePaneRect()
                : localPaneRect();
        QPen targetPen(
                m_DragTargetValid ? accent : QColor(220, 91, 91),
                2,
                Qt::DashLine);
        painter.setPen(targetPen);
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(targetPane.adjusted(2, 2, -2, -2), 7, 7);

        const QString sourceName = m_DragSourceLocal
                ? m_LocalEntries.value(m_DragSourceIndex).name
                : m_RemoteEntries.value(m_DragSourceIndex).name;
        QFont hintFont = painter.font();
        hintFont.setPointSize(9);
        hintFont.setBold(false);
        painter.setFont(hintFont);
        const QString hint = sourceName + QStringLiteral("\n") + m_DragHint;
        const QRect textBounds = painter.fontMetrics().boundingRect(
                QRect(0, 0, 360, 70),
                Qt::AlignLeft | Qt::AlignVCenter | Qt::TextWordWrap,
                hint);
        QSize bubbleSize(
                std::min(380, std::max(180, textBounds.width() + 26)),
                std::max(58, textBounds.height() + 18));
        QPoint bubbleTopLeft =
                m_DragPosition + QPoint(18, 18);
        bubbleTopLeft.setX(std::clamp(
                bubbleTopLeft.x(), 8, width() - bubbleSize.width() - 8));
        bubbleTopLeft.setY(std::clamp(
                bubbleTopLeft.y(), 8, height() - bubbleSize.height() - 8));
        const QRect bubble(bubbleTopLeft, bubbleSize);
        painter.setPen(border);
        painter.setBrush(QColor(38, 43, 51, 245));
        painter.drawRoundedRect(bubble, 8, 8);
        painter.setPen(m_DragTargetValid ? text : QColor(255, 145, 145));
        painter.drawText(
                bubble.adjusted(13, 8, -13, -8),
                Qt::AlignLeft | Qt::AlignVCenter | Qt::TextWordWrap,
                hint);
    }

    if (m_DialogVisible) {
        painter.fillRect(
                QRect(QPoint(0, 0), size()),
                QColor(0, 0, 0, 145));
        const QRect dialog = dialogRect();
        painter.setPen(border);
        painter.setBrush(QColor(31, 36, 43));
        painter.drawRoundedRect(dialog, 10, 10);

        painter.setPen(text);
        QFont titleFont = painter.font();
        titleFont.setBold(true);
        titleFont.setPointSize(12);
        painter.setFont(titleFont);
        painter.drawText(
                dialog.adjusted(24, 18, -24, -150),
                Qt::AlignLeft | Qt::AlignVCenter,
                m_DialogTitle);

        QFont dialogFont = painter.font();
        dialogFont.setBold(false);
        dialogFont.setPointSize(10);
        painter.setFont(dialogFont);
        if (m_DialogConfirmOnly) {
            painter.setPen(QColor(255, 174, 126));
            painter.drawText(
                    dialog.adjusted(24, 62, -24, -72),
                    Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
                    tr("\"%1\" will be permanently deleted. This cannot be undone.")
                            .arg(m_DialogText));
        }
        else {
            const QRect editor = dialog.adjusted(24, 72, -24, -82);
            painter.setPen(QColor(80, 93, 108));
            painter.setBrush(QColor(20, 24, 29));
            painter.drawRoundedRect(editor, 5, 5);
            painter.setPen(text);
            const QString shownText = m_DialogText + m_DialogPreedit;
            painter.drawText(
                    editor.adjusted(10, 0, -10, 0),
                    Qt::AlignLeft | Qt::AlignVCenter,
                    painter.fontMetrics().elidedText(
                            shownText, Qt::ElideLeft, editor.width() - 20));
            const int cursorX = editor.x() + 10 +
                    painter.fontMetrics().horizontalAdvance(shownText);
            painter.drawLine(
                    std::min(cursorX, editor.right() - 8),
                    editor.y() + 8,
                    std::min(cursorX, editor.right() - 8),
                    editor.bottom() - 8);
        }

        drawButton(
                dialogOkRect(),
                m_DialogConfirmOnly ? tr("Delete") : tr("OK"),
                true);
        drawButton(dialogCancelRect(), tr("Cancel"), true);
    }
}

void FileTransferWindow::mousePressEvent(QMouseEvent* event)
{
    const QPoint point = event->position().toPoint();
    if (m_DialogVisible) {
        if (event->button() == Qt::LeftButton) {
            if (dialogOkRect().contains(point)) {
                acceptDialog();
            }
            else if (dialogCancelRect().contains(point)) {
                closeDialog();
            }
        }
        event->accept();
        return;
    }

    if (event->button() != Qt::LeftButton) {
        QRasterWindow::mousePressEvent(event);
        return;
    }

    m_DragSourceIndex = -1;
    m_DragActive = false;
    m_DragHint.clear();
    if (conflictButtonRect().contains(point) && !m_Busy) {
        toggleConflictPolicy();
        return;
    }
    if (receiveDirectoryButtonRect().contains(point) && !m_Busy) {
        saveCurrentRemoteReceiveDirectory();
        return;
    }
    for (int index = 0; index < kActionCount; ++index) {
        if (actionButtonRect(true, index).contains(point)) {
            beginFileOperation(true, index);
            return;
        }
        if (actionButtonRect(false, index).contains(point)) {
            beginFileOperation(false, index);
            return;
        }
    }
    if (localPathRect().contains(point) && point.x() >= localPathRect().right() - 80) {
        localUp();
        return;
    }
    if (remotePathRect().contains(point) && point.x() >= remotePathRect().right() - 80) {
        remoteUp();
        return;
    }
    if (uploadButtonRect().contains(point)) {
        beginUpload();
        return;
    }
    if (downloadButtonRect().contains(point)) {
        beginDownload();
        return;
    }
    if (refreshButtonRect().contains(point) && !m_Busy) {
        refreshLocal();
        refreshRemote();
        return;
    }

    const int localIndex = rowAt(true, point);
    if (localIndex >= 0) {
        m_LocalSelection = localIndex;
        if (!m_LocalEntries.at(localIndex).drive && !m_Busy) {
            m_DragSourceLocal = true;
            m_DragSourceIndex = localIndex;
            m_DragStart = point;
            m_DragPosition = point;
        }
        update();
        return;
    }
    const int remoteIndex = rowAt(false, point);
    if (remoteIndex >= 0) {
        m_RemoteSelection = remoteIndex;
        if (!m_RemoteEntries.at(remoteIndex).drive && !m_Busy) {
            m_DragSourceLocal = false;
            m_DragSourceIndex = remoteIndex;
            m_DragStart = point;
            m_DragPosition = point;
        }
        update();
    }
}

void FileTransferWindow::mouseMoveEvent(QMouseEvent* event)
{
    if (m_DragSourceIndex < 0 ||
        m_Busy ||
        !(event->buttons() & Qt::LeftButton)) {
        QRasterWindow::mouseMoveEvent(event);
        return;
    }

    const QPoint point = event->position().toPoint();
    if (!m_DragActive) {
        const int dragDistance =
                QGuiApplication::styleHints()->startDragDistance();
        if ((point - m_DragStart).manhattanLength() < dragDistance) {
            return;
        }
        m_DragActive = true;
    }
    updateDrag(point);
}

void FileTransferWindow::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && m_DragActive) {
        const QPoint point = event->position().toPoint();
        finishDrag(point);
        resetDrag();
        event->accept();
        return;
    }
    m_DragSourceIndex = -1;
    QRasterWindow::mouseReleaseEvent(event);
}

void FileTransferWindow::mouseDoubleClickEvent(QMouseEvent* event)
{
    const QPoint point = event->position().toPoint();
    const int localIndex = rowAt(true, point);
    if (localIndex >= 0) {
        m_LocalSelection = localIndex;
        openLocalSelection();
        return;
    }
    const int remoteIndex = rowAt(false, point);
    if (remoteIndex >= 0) {
        m_RemoteSelection = remoteIndex;
        openRemoteSelection();
    }
}

void FileTransferWindow::wheelEvent(QWheelEvent* event)
{
    const int delta = event->angleDelta().y() > 0 ? -3 : 3;
    if (localPaneRect().contains(event->position().toPoint())) {
        m_LocalScroll += delta;
    }
    else if (remotePaneRect().contains(event->position().toPoint())) {
        m_RemoteScroll += delta;
    }
    clampScrollOffsets();
    update();
}

void FileTransferWindow::keyPressEvent(QKeyEvent* event)
{
    if (m_DialogVisible) {
        if (event->key() == Qt::Key_Escape) {
            closeDialog();
        }
        else if (event->key() == Qt::Key_Return ||
                 event->key() == Qt::Key_Enter) {
            acceptDialog();
        }
        else if (!m_DialogConfirmOnly &&
                 event->matches(QKeySequence::Paste)) {
            m_DialogText += QGuiApplication::clipboard()->text();
            update();
        }
        else if (!m_DialogConfirmOnly &&
                 event->key() == Qt::Key_Backspace) {
            m_DialogText.chop(1);
            update();
        }
        else if (!m_DialogConfirmOnly &&
                 !event->text().isEmpty() &&
                 !(event->modifiers() &
                   (Qt::ControlModifier | Qt::AltModifier |
                    Qt::MetaModifier))) {
            m_DialogText += event->text();
            update();
        }
        event->accept();
        return;
    }

    if (event->key() == Qt::Key_Escape) {
        hide();
    }
    else if (event->key() == Qt::Key_F5 && !m_Busy) {
        refreshLocal();
        refreshRemote();
    }
    else {
        QRasterWindow::keyPressEvent(event);
    }
}
