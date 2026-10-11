#include "collabtext/SyncManager.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <chrono>
#include <ctime>

namespace CollabText {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::string SyncManager::now_iso8601()
{
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&time_t, &tm);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

SyncManager::SyncManager(const Identity::Identity &identity,
                         const std::string &replica_id,
                         const std::string &device_name,
                         QObject *parent)
    : QObject(parent)
    , m_identity(identity)
    , m_replicaId(replica_id)
    , m_deviceName(device_name)
{
    connect(&m_timer, &QTimer::timeout, this, &SyncManager::syncCycle);
}

SyncManager::~SyncManager()
{
    stop();
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void SyncManager::start(const QString &sharedFolder)
{
    m_sharedFolder = sharedFolder;
    m_running = true;

    ensureDirectoryStructure();

    const std::filesystem::path folder = sharedFolder.toStdString();

    m_presence = std::make_unique<Identity::PresenceManager>(
        folder, m_replicaId, m_identity.identity_id);
    m_presence->start();

    m_projector = std::make_unique<Identity::IdentityProjector>(folder);

    // Publish our identity so peers can resolve us.
    m_projector->project(m_identity);

    m_timer.start(500); // 500ms sync cycle for ephemeral responsiveness
}

void SyncManager::stop()
{
    m_timer.stop();
    if (m_running) {
        if (m_presence)
            m_presence->depart();
        m_running = false;
    }
}

void SyncManager::setEphemeralState(const Identity::EphemeralState &state)
{
    m_ephemeralState = state;
}

// ---------------------------------------------------------------------------
// Sync cycle
// ---------------------------------------------------------------------------

void SyncManager::syncCycle()
{
    // Write our presence.
    Identity::Presence myPresence;
    myPresence.replica_id = m_replicaId;
    myPresence.identity_id = m_identity.identity_id;
    myPresence.device_name = m_deviceName;
    myPresence.active = true;
    myPresence.last_heartbeat = now_iso8601();
    m_presence->update_presence(myPresence);

    // Write our ephemeral state (bump sequence number + timestamp).
    m_ephemeralState.seq = ++m_ephemeralSeq;
    m_ephemeralState.timestamp = now_iso8601();
    m_presence->update_ephemeral(m_ephemeralState);
    m_presence->tick(std::chrono::steady_clock::now());

    // Read remote presences and collect live peers.
    const auto remotePrescences = m_presence->read_remote_presences();
    QList<Identity::Presence> livePeers;
    for (const auto &[rid, presence] : remotePrescences) {
        if (Identity::PresenceManager::is_live(presence))
            livePeers.append(presence);
    }

    // Read remote ephemerals and emit for live peers.
    const auto remoteEphems = m_presence->read_remote_ephemerals();
    for (const auto &[rid, ephState] : remoteEphems) {
        // Only emit for peers that are considered live
        bool peerIsLive = false;
        for (const auto &p : livePeers) {
            if (p.replica_id == rid) {
                peerIsLive = true;
                break;
            }
        }
        if (!peerIsLive)
            continue;

        // Look up identity for this peer
        Identity::Identity peerIdentity;
        if (m_projector) {
            // Find the identity_id from presence
            for (const auto &[presRid, pres] : remotePrescences) {
                if (presRid == rid) {
                    auto opt = m_projector->read(pres.identity_id);
                    if (opt)
                        peerIdentity = *opt;
                    break;
                }
            }
        }

        emit remoteEphemeralChanged(QString::fromStdString(rid), ephState, peerIdentity);
    }

    // Emit presence summary.
    emit presenceChanged(livePeers);
}

// ---------------------------------------------------------------------------
// Directory structure
// ---------------------------------------------------------------------------

void SyncManager::ensureDirectoryStructure()
{
    QDir dir(m_sharedFolder);
    dir.mkpath(QStringLiteral("meta"));
    dir.mkpath(QStringLiteral("identities"));

    // Ensure .stignore exists
    QString stignore = m_sharedFolder + QStringLiteral("/.stignore");
    if (!QFileInfo::exists(stignore)) {
        QFile f(stignore);
        if (f.open(QIODevice::WriteOnly)) {
            f.write("local/\n*.tmp\n*.part\n");
        }
    }
}

} // namespace CollabText
