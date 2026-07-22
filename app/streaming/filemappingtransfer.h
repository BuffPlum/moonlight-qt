#pragma once

#include "backend/nvcomputer.h"

#include <QMutex>
#include <QString>
#include <QStringList>
#include <QWaitCondition>

#include <atomic>
#include <memory>

namespace FileMappingTransfer {

struct State {
    std::atomic_bool stopRequested { false };
    QMutex lock;
    QWaitCondition finishedCondition;
    bool finished = false;
    QString outboxPath;
    QStringList events;
};

void start(NvComputer computer,
           QString mirrorRoot,
           std::shared_ptr<State> state,
           int timeoutMs = 10000);

// Returns false if the task did not finish within the requested timeout. A
// negative timeout waits until completion and is used before deleting paths
// that may still be referenced by the task.
bool stopAndWait(const std::shared_ptr<State>& state, int timeoutMs = 15000);

} // namespace FileMappingTransfer
