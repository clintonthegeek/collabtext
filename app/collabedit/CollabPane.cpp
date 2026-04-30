#include "CollabPane.h"

#include "crdt/SeedOp.h"
#include "ui/CollabPlainTextEdit.h"
#include "ui/MultiCursorController.h"
#include "ui/ParticipantListWidget.h"

#include <QHBoxLayout>
#include <QPlainTextDocumentLayout>
#include <QTextCursor>
#include <QTextDocument>
#include <QTimer>

#include <chrono>
#include <ctime>
#include <functional>
#include <optional>

using namespace CollabText;
using namespace CollabText::Crdt;
using namespace CollabText::Identity;
using namespace CollabText::Ui;

namespace CollabEdit {

namespace {

std::string now_iso8601() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

uint16_t replicaIdFromName(const std::string &replica_name) {
    std::hash<std::string> h;
    uint64_t v = h(replica_name);
    return static_cast<uint16_t>(v % 65535) + 1;  // avoid 0 (seed replica)
}

} // namespace

CollabPane::CollabPane(CollabText::Identity::Identity identity,
                       std::string replica_name,
                       std::filesystem::path sidecar_dir,
                       const std::string &seed_text,
                       QWidget *parent)
    : QWidget(parent)
    , m_identity(std::move(identity))
    , m_replicaName(std::move(replica_name))
    , m_buffer(replicaIdFromName(m_replicaName))
    , m_sync(m_buffer, sidecar_dir, m_replicaName)
    , m_presence(sidecar_dir, m_replicaName, m_identity.identity_id)
    , m_projector(sidecar_dir)
    , m_sessionStarted(now_iso8601())
{
    auto *root = new QHBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);

    m_edit = new CollabPlainTextEdit(this);
    m_qtDoc = new QTextDocument(this);
    m_qtDoc->setDocumentLayout(new QPlainTextDocumentLayout(m_qtDoc));
    m_qtDoc->setUndoRedoEnabled(false);
    m_edit->setDocument(m_qtDoc);
    root->addWidget(m_edit, 1);

    m_participantList = new ParticipantListWidget(this);
    m_participantList->setFixedWidth(220);
    root->addWidget(m_participantList);

    // 1) Apply seed deterministically.
    m_buffer.apply_ops({op_for_seed(seed_text)});

    // 2) Start FileSync (creates dirs, loads seq counters), then poll
    //    once to ingest any existing remote ops in the sidecar.
    m_sync.start();
    m_sync.poll();

    // 3) Render the resulting text into the QTextDocument as the
    //    initial content, then connect the change signal so future
    //    edits route to the buffer.
    m_syncing = true;
    m_qtDoc->setPlainText(QString::fromStdString(m_buffer.text()));
    m_syncing = false;

    connect(m_qtDoc, &QTextDocument::contentsChange,
            this, &CollabPane::onContentsChange);

    m_syncTimer = new QTimer(this);
    m_syncTimer->setInterval(100);
    connect(m_syncTimer, &QTimer::timeout, this, &CollabPane::syncCycle);
    m_syncTimer->start();
}

CollabPane::~CollabPane() {
    shutdown();
}

std::string CollabPane::text() const { return m_buffer.text(); }

void CollabPane::shutdown() {
    if (m_shutdown) return;
    m_shutdown = true;
    if (m_syncTimer) m_syncTimer->stop();
    m_sync.poll();
    m_presence.depart();
}

void CollabPane::onContentsChange(int position, int charsRemoved, int charsAdded) {
    if (m_syncing) return;

    std::string bufText = m_buffer.text();
    QString qBufText = QString::fromStdString(bufText);
    QString qtNow = m_qtDoc->toPlainText();

    uint32_t byteStart = qBufText.left(position).toUtf8().size();
    uint32_t byteEnd = byteStart;
    if (charsRemoved > 0) {
        byteEnd = qBufText.left(position + charsRemoved).toUtf8().size();
    }

    std::string inserted;
    if (charsAdded > 0) {
        QString insertedQt = qtNow.mid(position, charsAdded);
        inserted = insertedQt.toStdString();
    }

    auto op = m_buffer.apply_local_edit({{byteStart, byteEnd}}, {inserted});
    m_sync.push_local_op(op);
}

void CollabPane::syncCycle() {
    Global before = m_buffer.version();
    size_t applied = m_sync.poll();
    if (applied > 0) {
        auto edits = m_buffer.edits_since(before);
        m_syncing = true;
        QTextCursor cur(m_qtDoc);
        for (const auto &e : edits) {
            int qtStart = byteOffsetToQtPos(e.old_start);
            int qtEnd   = byteOffsetToQtPos(e.old_end);
            cur.setPosition(qtStart);
            cur.setPosition(qtEnd, QTextCursor::KeepAnchor);
            cur.insertText(QString::fromStdString(e.new_text));
        }
        m_syncing = false;
    }

    writePresence();
    writeEphemeral();
    applyRemoteCursors();
}

void CollabPane::writePresence() {
    Presence p;
    p.replica_id = m_replicaName;
    p.identity_id = m_identity.identity_id;
    p.device_name = m_replicaName;
    p.active = true;
    p.last_heartbeat = now_iso8601();
    p.session_started = m_sessionStarted;
    p.version_summary = m_buffer.version();
    m_presence.write_presence(p);
}

void CollabPane::writeEphemeral() {
    auto cursor = m_edit->textCursor();
    uint32_t bytePos = qtPosToByteOffset(cursor.position());
    uint32_t byteAnchor = qtPosToByteOffset(cursor.anchor());

    EphemeralState es;
    es.seq = ++m_ephemeralSeq;
    es.timestamp = now_iso8601();
    es.activity = "editing";
    auto posAnchor = m_buffer.anchor_at(bytePos, Bias::Right);
    auto selAnchor = m_buffer.anchor_at(byteAnchor, Bias::Left);
    es.cursors.push_back({selAnchor, posAnchor});

    m_presence.write_ephemeral(es);
}

void CollabPane::applyRemoteCursors() {
    auto remoteEphemerals = m_presence.read_remote_ephemerals();
    auto remotePresences  = m_presence.read_remote_presences();

    QList<RemoteCursor> cursors;
    std::vector<Presence> allPresences;

    for (const auto &[replicaId, p] : remotePresences) {
        allPresences.push_back(p);
    }
    Presence self;
    self.replica_id = m_replicaName;
    self.identity_id = m_identity.identity_id;
    self.active = true;
    self.last_heartbeat = now_iso8601();
    allPresences.push_back(self);

    for (const auto &[replicaId, es] : remoteEphemerals) {
        std::optional<CollabText::Identity::Identity> remoteIdentity;
        for (const auto &[prid, p] : remotePresences) {
            if (prid == replicaId) {
                auto maybe = m_projector.read(p.identity_id);
                if (maybe) remoteIdentity = *maybe;
                break;
            }
        }
        for (const auto &cp : es.cursors) {
            RemoteCursor rc;
            rc.bytePosition = m_buffer.resolve_anchor(cp.head);
            rc.byteAnchor   = m_buffer.resolve_anchor(cp.anchor);
            if (remoteIdentity) {
                rc.color = QColor(QString::fromStdString(remoteIdentity->color));
                rc.label = QString::fromStdString(remoteIdentity->display_name);
                rc.identityId = QString::fromStdString(remoteIdentity->identity_id);
            } else {
                rc.color = QColor(Qt::gray);
                rc.label = QString::fromStdString(replicaId);
            }
            rc.cursorVersion = (quint64(cp.head.replica_id) << 32) | cp.head.char_value;
            cursors.append(rc);
        }
    }
    m_edit->multiCursorController()->setRemoteCursors(cursors);

    auto identities = m_projector.read_all();
    m_participantList->updateParticipants(identities, allPresences);
}

uint32_t CollabPane::qtPosToByteOffset(int qtPos) const {
    QString docText = m_qtDoc->toPlainText();
    return docText.left(qMin(qtPos, docText.length())).toUtf8().size();
}

int CollabPane::byteOffsetToQtPos(uint32_t byteOffset) const {
    QString docText = m_qtDoc->toPlainText();
    QByteArray utf8 = docText.toUtf8();
    uint32_t clamped = qMin(byteOffset, static_cast<uint32_t>(utf8.size()));
    return QString::fromUtf8(utf8.data(), clamped).length();
}

} // namespace CollabEdit
