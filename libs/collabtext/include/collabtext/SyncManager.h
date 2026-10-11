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

// Presence and ephemeral-state helper. This class does not transport CRDT
// operations. Use DocumentSession with an OpStream for durable edits; the
// retained `engine` constructor parameter is ignored for source compatibility.
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
    void ensureDirectoryStructure();

    static std::string now_iso8601();

    QTimer m_timer;
    bool m_running = false;
    Identity::Identity m_identity;
    std::string m_replicaId;
    std::string m_deviceName;
    Identity::EphemeralState m_ephemeralState;
    uint64_t m_ephemeralSeq = 0;
    std::unique_ptr<Identity::PresenceManager> m_presence;
    std::unique_ptr<Identity::IdentityProjector> m_projector;
    QString m_sharedFolder;
};

} // namespace CollabText
