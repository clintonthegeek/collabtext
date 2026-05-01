#include "collabtext/SyncManager.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

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

SyncManager::SyncManager(CrdtEngine *engine,
                         const Identity::Identity &identity,
                         const std::string &replica_id,
                         const std::string &device_name,
                         QObject *parent)
    : QObject(parent)
    , m_engine(engine)
    , m_identity(identity)
    , m_replicaId(replica_id)
    , m_deviceName(device_name)
{
    connect(&m_timer, &QTimer::timeout, this, &SyncManager::syncCycle);

    // TODO: Re-enable when CrdtEngine gains serialization support.
    // Previously connected to YrsDocument::updateProduced to capture
    // outgoing updates for file-based sync.
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
    // 1. CRDT ops (stubs pending serialization support)
    flushLocalUpdates();
    readRemoteUpdates();

    // 2. Write our presence
    Identity::Presence myPresence;
    myPresence.replica_id = m_replicaId;
    myPresence.identity_id = m_identity.identity_id;
    myPresence.device_name = m_deviceName;
    myPresence.active = true;
    myPresence.last_heartbeat = now_iso8601();
    m_presence->update_presence(myPresence);

    // 3. Write our ephemeral state (bump sequence number + timestamp)
    m_ephemeralState.seq = ++m_ephemeralSeq;
    m_ephemeralState.timestamp = now_iso8601();
    m_presence->update_ephemeral(m_ephemeralState);
    m_presence->tick(std::chrono::steady_clock::now());

    // 4. Read remote presences — collect live peers
    const auto remotePrescences = m_presence->read_remote_presences();
    QList<Identity::Presence> livePeers;
    for (const auto &[rid, presence] : remotePrescences) {
        if (Identity::PresenceManager::is_live(presence))
            livePeers.append(presence);
    }

    // 5. Read remote ephemerals — emit per-peer signal for live peers
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

    // 6. Emit presence summary
    emit presenceChanged(livePeers);
}

// ---------------------------------------------------------------------------
// CRDT ops stubs
// ---------------------------------------------------------------------------

void SyncManager::flushLocalUpdates()
{
    if (m_pendingUpdates.isEmpty())
        return;

    // Write each update as a separate file in our replica's ops/ directory.
    // Filename is a monotonically increasing counter.
    QString opsDir = QStringLiteral("%1/replicas/%2/ops")
                         .arg(m_sharedFolder, QString::fromStdString(m_replicaId));

    // Read current sequence counter
    QString seqPath = QStringLiteral("%1/replicas/%2/seq")
                          .arg(m_sharedFolder, QString::fromStdString(m_replicaId));
    uint64_t seq = 0;
    QFile seqFile(seqPath);
    if (seqFile.open(QIODevice::ReadOnly))
        seq = seqFile.readAll().trimmed().toULongLong();

    for (const auto &update : m_pendingUpdates) {
        QString filename = QStringLiteral("%1/%2.bin").arg(opsDir).arg(seq++);
        QSaveFile f(filename);
        if (f.open(QIODevice::WriteOnly)) {
            f.write(update);
            f.commit();
        }
    }
    m_pendingUpdates.clear();

    // Write sequence counter atomically
    QSaveFile sf(seqPath);
    if (sf.open(QIODevice::WriteOnly)) {
        sf.write(QByteArray::number(static_cast<qulonglong>(seq)));
        sf.commit();
    }
}

void SyncManager::readRemoteUpdates()
{
    const QString replicasDirPath = m_sharedFolder + QStringLiteral("/replicas");
    QDir replicasDir(replicasDirPath);
    if (!replicasDir.exists())
        return;

    const QString myReplica = QString::fromStdString(m_replicaId);

    for (const auto &entry : replicasDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (entry == myReplica)
            continue; // skip our own directory

        QString opsDir = replicasDir.filePath(entry) + QStringLiteral("/ops");
        QDir ops(opsDir);
        if (!ops.exists())
            continue;

        // Read our local cursor for this peer
        QString cursorPath = QStringLiteral("%1/local/%2/cursors/%3")
                                 .arg(m_sharedFolder, myReplica, entry);
        uint64_t readUpTo = 0;
        QFile cursorFile(cursorPath);
        if (cursorFile.open(QIODevice::ReadOnly))
            readUpTo = cursorFile.readAll().trimmed().toULongLong();

        // Read peer's sequence counter
        QString peerSeqPath = replicasDir.filePath(entry) + QStringLiteral("/seq");
        uint64_t peerSeq = 0;
        QFile peerSeqFile(peerSeqPath);
        if (peerSeqFile.open(QIODevice::ReadOnly))
            peerSeq = peerSeqFile.readAll().trimmed().toULongLong();

        if (peerSeq <= readUpTo)
            continue; // nothing new

        // TODO: Re-enable when CrdtEngine gains serialization support.
        // Previously read .bin files and called m_engine->applyUpdate(update).
        // For now, skip applying remote updates.

        // Update cursor
        QDir().mkpath(QFileInfo(cursorPath).path());
        QSaveFile cf(cursorPath);
        if (cf.open(QIODevice::WriteOnly)) {
            cf.write(QByteArray::number(static_cast<qulonglong>(peerSeq)));
            cf.commit();
        }
    }
}

// ---------------------------------------------------------------------------
// Directory structure
// ---------------------------------------------------------------------------

void SyncManager::ensureDirectoryStructure()
{
    const QString myReplica = QString::fromStdString(m_replicaId);
    QDir dir(m_sharedFolder);
    dir.mkpath(QStringLiteral("replicas/%1/ops").arg(myReplica));
    dir.mkpath(QStringLiteral("local/%1/cursors").arg(myReplica));
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
