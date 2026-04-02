#pragma once

#include "collabtext/YrsWrapper.h"

#include <QDir>
#include <QJsonObject>
#include <QObject>
#include <QTimer>

namespace CollabText {

// Manages file-based sync for a CollabDocument per CRDT_SYNC_SPEC.md.
//
// Each replica writes its updates to its own directory in the shared
// folder. The sync loop reads updates from other replicas' directories.
// Syncthing (or equivalent) propagates the files.
//
// This is the "floor" transport. Direct channels layer on top.
class SyncManager : public QObject {
    Q_OBJECT

public:
    explicit SyncManager(YrsDocument *crdt, QObject *parent = nullptr);
    ~SyncManager() override;

    // Initialize with path to the shared folder and our replica ID.
    void start(const QString &sharedFolder, const QString &replicaId);
    void stop();

    void setEphemeralState(const QJsonObject &state);

    bool isRunning() const { return m_running; }
    QString replicaId() const { return m_replicaId; }

signals:
    void syncError(const QString &message);
    void remoteEphemeralChanged(const QString &replicaId, const QJsonObject &state);

private slots:
    void syncCycle();

private:
    void flushLocalUpdates();
    void readRemoteUpdates();
    void writeEphemeral();
    void readRemoteEphemerals();
    void ensureDirectoryStructure();

    YrsDocument *m_crdt;
    QTimer m_timer;
    QString m_sharedFolder;
    QString m_replicaId;
    bool m_running = false;

    // Pending local updates not yet flushed to disk
    QList<QByteArray> m_pendingUpdates;
    QJsonObject m_ephemeralState;
};

} // namespace CollabText
