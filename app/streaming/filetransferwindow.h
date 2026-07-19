#pragma once

#include <atomic>
#include <memory>

#include <QMutex>
#include <QImage>
#include <QInputMethodEvent>
#include <QPoint>
#include <QRasterWindow>
#include <QThread>
#include <QVariantList>
#include <QVector>

#include "backend/nvcomputer.h"

class FileMappingProtocolAdapter;

class FileTransferWorker : public QObject
{
    Q_OBJECT

public:
    explicit FileTransferWorker(NvComputer computer);
    ~FileTransferWorker() override;

    void requestCancel();

public slots:
    void initialize();
    void browseRemote(const QString& mappingId, const QString& path);
    void upload(const QString& localPath,
                const QString& mappingId,
                const QString& remoteDirectory,
                int conflictPolicy);
    void download(const QString& mappingId,
                  const QString& remotePath,
                  bool directory,
                  const QString& localDirectory,
                  int conflictPolicy);
    void createRemoteFile(const QString& mappingId,
                          const QString& remotePath,
                          int conflictPolicy);
    void createRemoteFolder(const QString& mappingId,
                            const QString& remotePath,
                            int conflictPolicy);
    void renameRemote(const QString& mappingId,
                      const QString& remotePath,
                      const QString& destinationPath);
    void deleteRemote(const QString& mappingId,
                      const QString& remotePath,
                      bool recursive);

signals:
    void remoteReady(const QVariantList& mappings, const QString& error);
    void remoteListed(const QString& mappingId,
                      const QString& path,
                      const QVariantList& entries,
                      const QString& error);
    void transferProgress(const QString& message, quint64 completed, quint64 total);
    void transferFinished(bool ok, const QString& message);
    void operationFinished(bool ok, const QString& message);

private:
    bool ensureConnected(QString& error);
    bool uploadItem(const QString& localPath,
                     const QString& mappingId,
                     const QString& remotePath,
                     int conflictPolicy,
                     QString& error);
    bool uploadFile(const QString& localPath,
                     const QString& mappingId,
                     const QString& remotePath,
                     int conflictPolicy,
                     QString& error);
    bool downloadItem(const QString& mappingId,
                      const QString& remotePath,
                      bool directory,
                      const QString& localPath,
                      QString& error);
    bool downloadFile(const QString& mappingId,
                      const QString& remotePath,
                      const QString& localPath,
                      QString& error);

    NvComputer m_Computer;
    std::unique_ptr<FileMappingProtocolAdapter> m_Client;
    std::atomic_bool m_Cancelled { false };
};

class FileTransferWindow : public QRasterWindow
{
    Q_OBJECT

public:
    explicit FileTransferWindow(NvComputer computer);
    ~FileTransferWindow() override;

    void showAndActivate();
    // SDL emits one event per dropped path. Queueing keeps those files in one
    // background transfer stream instead of opening parallel host sessions.
    void queueExternalUpload(const QString& localPath);

signals:
    void initializeWorker();
    void requestRemoteList(const QString& mappingId, const QString& path);
    void requestUpload(const QString& localPath,
                       const QString& mappingId,
                       const QString& remoteDirectory,
                       int conflictPolicy);
    void requestDownload(const QString& mappingId,
                         const QString& remotePath,
                         bool directory,
                         const QString& localDirectory,
                         int conflictPolicy);
    void requestCreateRemoteFile(const QString& mappingId,
                                 const QString& remotePath,
                                 int conflictPolicy);
    void requestCreateRemoteFolder(const QString& mappingId,
                                   const QString& remotePath,
                                   int conflictPolicy);
    void requestRenameRemote(const QString& mappingId,
                             const QString& remotePath,
                             const QString& destinationPath);
    void requestDeleteRemote(const QString& mappingId,
                             const QString& remotePath,
                             bool recursive);

protected:
    bool event(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private slots:
    void onRemoteReady(const QVariantList& mappings, const QString& error);
    void onRemoteListed(const QString& mappingId,
                        const QString& path,
                        const QVariantList& entries,
                        const QString& error);
    void onTransferProgress(const QString& message, quint64 completed, quint64 total);
    void onTransferFinished(bool ok, const QString& message);
    void onOperationFinished(bool ok, const QString& message);

private:
    struct Entry {
        QString name;
        QString path;
        QString mappingId;
        bool directory = false;
        bool drive = false;
        bool writable = false;
        bool deletable = false;
        quint64 size = 0;
        QImage icon;
    };

    QRect localPaneRect() const;
    QRect remotePaneRect() const;
    QRect localPathRect() const;
    QRect remotePathRect() const;
    QRect uploadButtonRect() const;
    QRect downloadButtonRect() const;
    QRect refreshButtonRect() const;
    QRect conflictButtonRect() const;
    QRect receiveDirectoryButtonRect() const;
    QRect actionButtonRect(bool local, int actionIndex) const;
    QRect dialogRect() const;
    QRect dialogOkRect() const;
    QRect dialogCancelRect() const;
    QRect rowRect(bool local, int visibleRow) const;
    int rowAt(bool local, const QPoint& point) const;
    int visibleRowCount() const;

    void loadLocalRoot();
    void browseLocal(const QString& path);
    void refreshLocal();
    void refreshRemote();
    void openLocalSelection();
    void openRemoteSelection();
    void localUp();
    void remoteUp();
    void beginUpload();
    void beginDownload();
    void beginFileOperation(bool local, int actionIndex);
    void showNameDialog(bool local, int actionIndex, const QString& initialText = QString());
    void showDeleteDialog(bool local);
    void acceptDialog();
    void closeDialog();
    void saveCurrentRemoteReceiveDirectory();
    bool configuredRemoteDestination(QString& mappingId,
                                     QString& remoteDirectory,
                                     QString& displayPath) const;
    void startNextExternalUpload();
    int conflictPolicyValue() const;
    void toggleConflictPolicy();
    bool resolveRemoteDropTarget(const QPoint& point,
                                 QString& mappingId,
                                 QString& remoteDirectory,
                                 QString& displayPath) const;
    bool resolveLocalDropTarget(const QPoint& point,
                                QString& localDirectory) const;
    void updateDrag(const QPoint& point);
    void finishDrag(const QPoint& point);
    void resetDrag();
    void clampScrollOffsets();
    void setStatus(const QString& status, bool error = false);

    QThread m_WorkerThread;
    FileTransferWorker* m_Worker = nullptr;
    QVector<Entry> m_LocalEntries;
    QVector<Entry> m_RemoteEntries;
    QVector<Entry> m_RemoteRoots;
    QString m_LocalPath;
    QString m_RemoteMappingId;
    QString m_RemoteMappingName;
    QString m_RemotePath;
    QString m_Status;
    QString m_DragHint;
    bool m_StatusError = false;
    bool m_Busy = true;
    bool m_RemoteWritable = false;
    bool m_RemoteDeleteAllowed = false;
    bool m_ProgressVisible = false;
    int m_ProgressPercent = 0;
    int m_LocalSelection = -1;
    int m_RemoteSelection = -1;
    int m_LocalScroll = 0;
    int m_RemoteScroll = 0;
    bool m_DragSourceLocal = true;
    bool m_DragActive = false;
    bool m_DragTargetValid = false;
    int m_DragSourceIndex = -1;
    QPoint m_DragStart;
    QPoint m_DragPosition;
    QStringList m_ExternalUploadQueue;
    bool m_ExternalUploadActive = false;

    // The raster window avoids a Qt Widgets dependency, so lightweight name
    // and delete prompts are painted and edited directly in this window.
    bool m_DialogVisible = false;
    bool m_DialogConfirmOnly = false;
    bool m_DialogLocal = true;
    int m_DialogAction = -1;
    QString m_DialogTitle;
    QString m_DialogText;
    QString m_DialogPreedit;
};
