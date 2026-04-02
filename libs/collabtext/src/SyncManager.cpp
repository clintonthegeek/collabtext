#include "collabtext/SyncManager.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace CollabText {

SyncManager::SyncManager(CrdtEngine *engine, QObject *parent)
    : QObject(parent)
    , m_engine(engine)
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

void SyncManager::start(const QString &sharedFolder, const QString &replicaId)
{
    m_sharedFolder = sharedFolder;
    m_replicaId = replicaId;
    m_running = true;

    ensureDirectoryStructure();
    m_timer.start(500); // 500ms sync cycle for ephemeral responsiveness
}

void SyncManager::stop()
{
    m_timer.stop();
    if (m_running) {
        flushLocalUpdates();
        m_running = false;
    }
}

void SyncManager::syncCycle()
{
    flushLocalUpdates();
    readRemoteUpdates();
    writeEphemeral();
    readRemoteEphemerals();
}

void SyncManager::flushLocalUpdates()
{
    if (m_pendingUpdates.isEmpty())
        return;

    // Write each update as a separate file in our replica's ops/ directory.
    // Filename is a monotonically increasing counter.
    QString opsDir = QStringLiteral("%1/replicas/%2/ops")
                         .arg(m_sharedFolder, m_replicaId);

    // Read current sequence counter
    QString seqPath = QStringLiteral("%1/replicas/%2/seq")
                          .arg(m_sharedFolder, m_replicaId);
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
    QDir replicasDir(m_sharedFolder + QStringLiteral("/replicas"));
    if (!replicasDir.exists())
        return;

    for (const auto &entry : replicasDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (entry == m_replicaId)
            continue; // skip our own directory

        QString opsDir = replicasDir.filePath(entry) + QStringLiteral("/ops");
        QDir ops(opsDir);
        if (!ops.exists())
            continue;

        // Read our local cursor for this peer
        QString cursorPath = QStringLiteral("%1/local/%2/cursors/%3")
                                 .arg(m_sharedFolder, m_replicaId, entry);
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
        // Previously read .bin files and called m_crdt->applyUpdate(update).
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

void SyncManager::setEphemeralState(const QJsonObject &state)
{
    m_ephemeralState = state;
}

void SyncManager::writeEphemeral()
{
    QJsonObject obj = m_ephemeralState;
    obj[QStringLiteral("last_heartbeat")] =
        QDateTime::currentDateTimeUtc().toMSecsSinceEpoch();

    QString path = QStringLiteral("%1/replicas/%2/ephemeral.json")
                       .arg(m_sharedFolder, m_replicaId);
    QSaveFile f(path);
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(obj).toJson(QJsonDocument::Compact));
        f.commit();
    }
}

void SyncManager::readRemoteEphemerals()
{
    QDir replicasDir(m_sharedFolder + QStringLiteral("/replicas"));
    if (!replicasDir.exists())
        return;

    const qint64 now = QDateTime::currentDateTimeUtc().toMSecsSinceEpoch();

    for (const auto &entry : replicasDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (entry == m_replicaId)
            continue;

        QString path = replicasDir.filePath(entry) + QStringLiteral("/ephemeral.json");
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly))
            continue;

        QJsonParseError err;
        QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject())
            continue;

        QJsonObject obj = doc.object();
        qint64 heartbeat = static_cast<qint64>(
            obj.value(QStringLiteral("last_heartbeat")).toDouble());
        if (now - heartbeat > 5000)
            continue; // stale — peer likely offline

        emit remoteEphemeralChanged(entry, obj);
    }
}

void SyncManager::ensureDirectoryStructure()
{
    QDir dir(m_sharedFolder);
    dir.mkpath(QStringLiteral("replicas/%1/ops").arg(m_replicaId));
    dir.mkpath(QStringLiteral("local/%1/cursors").arg(m_replicaId));
    dir.mkpath(QStringLiteral("meta"));

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
