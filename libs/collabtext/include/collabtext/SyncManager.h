#pragma once

#include "collabtext/CrdtEngine.h"
#include "collabtext/Identity.h"
#include "collabtext/IdentityProjector.h"
#include "collabtext/PresenceManager.h"

#include <QList>
#include <QObject>
#include <QString>
#include <QTimer>

#include <memory>
#include <string>

namespace CollabText {

// Manages file-based sync for a CollabDocument per CRDT_SYNC_SPEC.md.
//
// Each replica writes its updates to its own directory in the shared
// folder. The sync loop reads updates from other replicas' directories.
// Syncthing (or equivalent) propagates the files.
//
// This is the "floor" transport. Direct channels layer on top.
//
// NOTE: CRDT ops sync is currently disabled pending serialization support in
// the native C++ CRDT engine. The file I/O structure is preserved for when
// serialization is implemented.
//
// Presence and ephemeral state are fully active and delegate to PresenceManager
// and IdentityProjector from the core layer.
class SyncManager : public QObject {
    Q_OBJECT

public:
    SyncManager(CrdtEngine *engine,
                const Identity::Identity &identity,
                const std::string &replica_id,
                const std::string &device_name,
                QObject *parent = nullptr);
    ~SyncManager() override;

    // Initialize with path to the shared folder. replica_id comes from constructor.
    void start(const QString &sharedFolder);
    void stop();

    void setEphemeralState(const Identity::EphemeralState &state);

    bool isRunning() const { return m_running; }
    QString replicaId() const { return QString::fromStdString(m_replicaId); }

signals:
    void remoteEphemeralChanged(const QString &replicaId,
                                const Identity::EphemeralState &state,
                                const Identity::Identity &identity);
    void presenceChanged(const QList<Identity::Presence> &livePeers);
    void syncError(const QString &message);

private slots:
    void syncCycle();

private:
    void flushLocalUpdates();
    void readRemoteUpdates();
    void ensureDirectoryStructure();

    static std::string now_iso8601();

    CrdtEngine *m_engine;
    QTimer m_timer;
    bool m_running = false;
    Identity::Identity m_identity;
    std::string m_replicaId;
    std::string m_deviceName;
    Identity::EphemeralState m_ephemeralState;
    uint64_t m_ephemeralSeq = 0;
    std::unique_ptr<Identity::PresenceManager> m_presence;
    std::unique_ptr<Identity::IdentityProjector> m_projector;
    QList<QByteArray> m_pendingUpdates;
    QString m_sharedFolder;
};

} // namespace CollabText
