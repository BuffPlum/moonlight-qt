#pragma once

#include "backend/nvcomputer.h"
#include "video/overlaymenupanel.h"

#include <QMutex>
#include <QString>
#include <QWaitCondition>

#include <atomic>
#include <memory>

namespace FileMappingUx {

struct ProbeState {
    QMutex lock;
    bool pending = false;
    bool available = false;
    bool error = false;
    bool fullDiskAccess = false;
    QString detail;
    QString message;
    QString diagnosticsPath;
};

struct MountState {
    std::atomic_bool stopRequested { false };
    QMutex lock;
    QWaitCondition finishedCondition;
    bool finished = false;
    bool pending = false;
    bool ok = false;
    QString detail;
    QString message;
    QString displayPath;
    QString diagnosticsPath;
};

QString stateName(OverlayMenuPanel::FileMappingState state);
QString diagnosticsDirectory();
QString diagnosticsPath();
QString appendDiagnostic(const QString& event,
                         const QString& detail = QString(),
                         const QString& hostUuid = QString(),
                         const QString& sessionId = QString());

void startCapabilityProbe(NvComputer computer,
                          std::shared_ptr<ProbeState> state,
                          int timeoutMs);
void startMount(NvComputer computer,
                QString sessionId,
                std::shared_ptr<MountState> state,
                int timeoutMs);
// Stops a pending mount before its provider can outlive the streaming session.
// A negative timeout waits until the task has completed its provider cleanup.
bool stopMountAndWait(const std::shared_ptr<MountState>& state,
                      int timeoutMs = 15000);

} // namespace FileMappingUx
