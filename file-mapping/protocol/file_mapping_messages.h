#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>

#include "file_mapping_errors.h"

namespace FileMapping {

// Transfers resolve a destination collision on the machine that owns the
// destination, which avoids choosing a stale "(n)" suffix on the other peer.
enum class ConflictPolicy {
    KeepBoth,
    Overwrite,
    Reject,
};

inline QString conflictPolicyName(ConflictPolicy policy)
{
    switch (policy) {
    case ConflictPolicy::KeepBoth:
        return QStringLiteral("keep_both");
    case ConflictPolicy::Overwrite:
        return QStringLiteral("overwrite");
    case ConflictPolicy::Reject:
        return QStringLiteral("reject");
    }
    return QStringLiteral("keep_both");
}

struct Capability {
    bool available = false;
    bool enabled = false;
    bool listening = false;
    quint16 port = 0;
    QString sessionEndpoint;
    QString sessionUrl;
    QString sessionToken;
    QString clientUuid;
    Error error;
};

struct RemoteMapping {
    QString id;
    QString displayName;
    QString volumeLabel;
    QString side;
    QString mode;
    QStringList capabilities;
};

struct RemoteEntry {
    QString mappingId;
    QString path;
    QString displayName;
    bool directory = false;
    quint64 size = 0;
    QDateTime modifiedAt;
};

struct RemoteStat {
    bool exists = false;
    bool directory = false;
    quint64 size = 0;
    QDateTime modifiedAt;
};

struct ListResult {
    Error error;
    QList<RemoteEntry> entries;

    bool ok() const { return error.ok(); }
};

struct StatResult {
    Error error;
    RemoteStat stat;

    bool ok() const { return error.ok(); }
};

struct ReadResult {
    Error error;
    QByteArray data;

    bool ok() const { return error.ok(); }
};

struct WriteResult {
    Error error;
    quint64 nextOffset = 0;
    bool completed = false;
    // Sunshine may choose an Explorer-style suffixed name for KeepBoth.
    QString actualPath;

    bool ok() const { return error.ok(); }
};

struct PathResult {
    Error error;
    // mkdir/write may return a suffixed path when KeepBoth is selected.
    QString actualPath;
    bool created = false;

    bool ok() const { return error.ok(); }
};

} // namespace FileMapping
