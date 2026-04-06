#include <QApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QPushButton>
#include <QRandomGenerator>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTimer>
#include <QVBoxLayout>
#include <QTextCursor>
#include <QPlainTextDocumentLayout>

#include <chrono>
#include <ctime>

#include "crdt/Buffer.h"
#include "crdt/FileSync.h"
#include "ui/CollabPlainTextEdit.h"
#include "ui/MultiCursorController.h"
#include "ui/ParticipantListWidget.h"

#include "collabtext/Identity.h"
#include "collabtext/IdentityStore.h"
#include "collabtext/PresenceManager.h"
#include "collabtext/IdentityProjector.h"

using namespace CollabText::Crdt;
using namespace CollabText::Ui;
using namespace CollabText::Identity;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::string now_iso8601() {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&time_t, &tm);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

/// A single editor pane with its own CRDT Buffer, FileSync, and identity.
///
/// Integration pattern (following y-codemirror6 binding):
/// - Local edits: QTextDocument contentsChange -> Buffer::apply_local_edit
/// - Remote edits: Buffer::apply_ops -> edits_since() -> surgical QTextCursor ops
/// - No full document replacement. Ever.
class EditorPane : public QWidget {
    Q_OBJECT
public:
    EditorPane(const Identity &identity, uint16_t replicaId,
               const std::string &replicaName,
               const std::filesystem::path &sharedFolder,
               QWidget *parent = nullptr)
        : QWidget(parent)
        , m_identity(identity)
        , m_replicaName(replicaName)
        , m_buffer(replicaId)
        , m_sync(m_buffer, sharedFolder, replicaName)
        , m_presence(sharedFolder, replicaName, identity.identity_id)
        , m_edit(new CollabPlainTextEdit(this))
        , m_qtDoc(new QTextDocument(this))
    {
        m_qtDoc->setDocumentLayout(new QPlainTextDocumentLayout(m_qtDoc));
        m_qtDoc->setUndoRedoEnabled(false);
        m_edit->setDocument(m_qtDoc);

        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);

        QColor headerColor(QString::fromStdString(identity.color));
        auto *header = new QLabel(QString::fromStdString(identity.display_name), this);
        header->setStyleSheet(QStringLiteral("font-weight: bold; color: %1;")
                                  .arg(headerColor.name()));
        layout->addWidget(header);
        layout->addWidget(m_edit);

        m_edit->setPlaceholderText(
            QStringLiteral("Type here... (Alt+Click for multi-cursor, "
                           "Ctrl+Alt+Up/Down to add cursors)"));

        m_statusLabel = new QLabel(this);
        auto *gremlinBtn = new QPushButton(QStringLiteral("Gremlin: OFF"), this);
        gremlinBtn->setCheckable(true);
        auto *bottomRow = new QHBoxLayout;
        bottomRow->addWidget(m_statusLabel);
        bottomRow->addStretch();
        bottomRow->addWidget(gremlinBtn);
        layout->addLayout(bottomRow);

        m_gremlinTimer = new QTimer(this);
        m_gremlinTimer->setInterval(80);
        connect(m_gremlinTimer, &QTimer::timeout, this, &EditorPane::gremlinTick);
        connect(gremlinBtn, &QPushButton::toggled, this, [this, gremlinBtn](bool on) {
            if (on) {
                m_gremlinTimer->start();
                gremlinBtn->setText(QStringLiteral("Gremlin: ON"));
            } else {
                m_gremlinTimer->stop();
                gremlinBtn->setText(QStringLiteral("Gremlin: OFF"));
            }
        });

        connect(m_edit->multiCursorController(),
                &MultiCursorController::cursorsChanged, this,
                [this]() {
                    int n = m_edit->multiCursorController()->cursorCount();
                    m_statusLabel->setText(
                        n > 1 ? QStringLiteral("%1 cursors").arg(n)
                              : QStringLiteral("1 cursor"));
                });

        // Local edits: QTextDocument -> Buffer -> FileSync
        connect(m_qtDoc, &QTextDocument::contentsChange,
                this, &EditorPane::onContentsChange);

        m_sync.start();
    }

    CollabPlainTextEdit *editor() const { return m_edit; }
    Buffer &buffer() { return m_buffer; }
    const Identity &identity() const { return m_identity; }
    PresenceManager &presenceManager() { return m_presence; }
    const std::string &replicaName() const { return m_replicaName; }

    /// Run one sync cycle. Saves version before polling, then applies
    /// remote edits surgically to the QTextDocument via edits_since().
    void poll() {
        Global before = m_buffer.version();
        size_t applied = m_sync.poll();
        if (applied > 0) {
            applyEditsToQt(m_buffer.edits_since(before));
        }
    }

    /// Write presence.json for this pane's replica.
    void writePresence() {
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

    /// Write ephemeral.json with cursor anchors.
    void writeEphemeral(uint64_t seq) {
        auto cursor = m_edit->textCursor();
        uint32_t bytePos = qtPosToByteOffset(cursor.position());
        uint32_t byteAnchor = qtPosToByteOffset(cursor.anchor());

        EphemeralState es;
        es.seq = seq;
        es.timestamp = now_iso8601();
        es.activity = "editing";

        auto posAnchor = m_buffer.anchor_at(bytePos, Bias::Right);
        auto selAnchor = m_buffer.anchor_at(byteAnchor, Bias::Left);
        es.cursors.push_back({selAnchor, posAnchor});

        m_presence.write_ephemeral(es);
    }

    /// Apply a remote EphemeralState, resolving anchors and setting remote cursors.
    void applyRemoteEphemeral(const EphemeralState &es, const Identity &remoteIdentity) {
        QList<RemoteCursor> cursors;
        for (const auto &cp : es.cursors) {
            RemoteCursor rc;
            rc.bytePosition = m_buffer.resolve_anchor(cp.head);
            rc.byteAnchor = m_buffer.resolve_anchor(cp.anchor);
            rc.color = QColor(QString::fromStdString(remoteIdentity.color));
            rc.label = QString::fromStdString(remoteIdentity.display_name);
            rc.identityId = QString::fromStdString(remoteIdentity.identity_id);
            rc.cursorVersion = (quint64(cp.head.replica_id) << 32) | cp.head.char_value;
            cursors.append(rc);
        }
        m_edit->multiCursorController()->setRemoteCursors(cursors);
    }

    /// Convert a Qt character position to a byte offset.
    uint32_t qtPosToByteOffset(int qtPos) const {
        QString docText = m_qtDoc->toPlainText();
        return docText.left(qMin(qtPos, docText.length())).toUtf8().size();
    }

    /// Convert a byte offset to a Qt character position.
    int byteOffsetToQtPos(uint32_t byteOffset) const {
        QString docText = m_qtDoc->toPlainText();
        QByteArray utf8 = docText.toUtf8();
        uint32_t clamped = qMin(byteOffset, static_cast<uint32_t>(utf8.size()));
        return QString::fromUtf8(utf8.data(), clamped).length();
    }

private slots:
    void onContentsChange(int position, int charsRemoved, int charsAdded) {
        if (m_syncing) return;

        std::string bufText = m_buffer.text();
        QString qBufText = QString::fromStdString(bufText);

        uint32_t byteStart = qBufText.left(position).toUtf8().size();
        uint32_t byteEnd = byteStart;
        if (charsRemoved > 0) {
            byteEnd = qBufText.left(position + charsRemoved).toUtf8().size();
        }

        std::string inserted;
        if (charsAdded > 0) {
            QTextCursor cursor(m_qtDoc);
            cursor.setPosition(position);
            cursor.setPosition(position + charsAdded, QTextCursor::KeepAnchor);
            QString sel = cursor.selectedText();
            sel.replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
            inserted = sel.toStdString();
        }

        auto op = m_buffer.apply_local_edit({{byteStart, byteEnd}}, {inserted});
        m_sync.push_local_op(op);
    }

    void gremlinTick() {
        static const char *lorem[] = {
            "lorem ", "ipsum ", "dolor ", "sit ", "amet ", "consectetur ",
            "adipiscing ", "elit ", "sed ", "do ", "eiusmod ", "tempor ",
            "incididunt ", "ut ", "labore ", "et ", "dolore ", "magna ",
            "aliqua ", "enim ", "ad ", "minim ", "veniam ", "quis ",
            "nostrud ", "exercitation ", "ullamco ", "laboris ", "nisi ",
        };
        static constexpr int nwords = sizeof(lorem) / sizeof(lorem[0]);

        auto *rng = QRandomGenerator::global();
        uint32_t docLen = m_buffer.visible_length();
        int roll = rng->bounded(50);

        Global before = m_buffer.version();
        Operation op;

        if (roll == 0 && docLen > 10) {
            uint32_t delLen = qMin(static_cast<uint32_t>(rng->bounded(3, 9)), docLen);
            uint32_t pos = rng->bounded(docLen - delLen + 1);
            op = m_buffer.apply_local_edit({{pos, pos + delLen}}, {""});
        } else if (roll < 3 && docLen > 0) {
            uint32_t pos = rng->bounded(docLen + 1);
            op = m_buffer.apply_local_edit({{pos, pos}}, {"\n"});
        } else {
            uint32_t pos = docLen;
            if (rng->bounded(5) == 0 && docLen > 0)
                pos = rng->bounded(docLen + 1);
            const char *word = lorem[rng->bounded(nwords)];
            op = m_buffer.apply_local_edit({{pos, pos}}, {word});
        }

        m_sync.push_local_op(op);
        applyEditsToQt(m_buffer.edits_since(before));
    }

private:
    void applyEditsToQt(const std::vector<TextEdit> &edits) {
        if (edits.empty()) return;
        m_syncing = true;

        for (auto it = edits.rbegin(); it != edits.rend(); ++it) {
            int qtStart = byteOffsetToQtPos(it->old_start);
            int qtEnd = byteOffsetToQtPos(it->old_end);
            QTextCursor cursor(m_qtDoc);
            cursor.setPosition(qtStart);
            if (qtEnd > qtStart)
                cursor.setPosition(qtEnd, QTextCursor::KeepAnchor);
            QString replacement = QString::fromUtf8(
                it->new_text.data(),
                static_cast<int>(it->new_text.size()));
            cursor.insertText(replacement);
        }

        m_syncing = false;
    }

    bool m_syncing = false;
    Identity m_identity;
    std::string m_replicaName;
    std::string m_sessionStarted = now_iso8601();
    Buffer m_buffer;
    FileSync m_sync;
    PresenceManager m_presence;
    CollabPlainTextEdit *m_edit;
    QTextDocument *m_qtDoc;
    QLabel *m_statusLabel;
    QTimer *m_gremlinTimer = nullptr;
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(const std::filesystem::path &sharedFolder,
                        const Identity &aliceId,
                        const Identity &bobId)
        : m_projector(sharedFolder)
    {
        setWindowTitle(QStringLiteral("CollabText \u2014 Full-Stack Demo"));
        resize(1200, 600);

        // Project identities into the shared folder so each pane can look
        // up the other's display_name and color.
        m_projector.project(aliceId);
        m_projector.project(bobId);

        auto *central = new QWidget(this);
        auto *layout = new QHBoxLayout(central);

        m_paneA = new EditorPane(aliceId, 1, "alice", sharedFolder, central);
        m_paneB = new EditorPane(bobId, 2, "bob", sharedFolder, central);
        layout->addWidget(m_paneA, 1);
        layout->addWidget(m_paneB, 1);

        m_participantList = new ParticipantListWidget(central);
        m_participantList->setFixedWidth(200);
        layout->addWidget(m_participantList);

        setCentralWidget(central);

        auto *syncTimer = new QTimer(this);
        connect(syncTimer, &QTimer::timeout, this, &MainWindow::syncCycle);
        syncTimer->start(100);

        statusBar()->showMessage(
            QStringLiteral("Full-stack sync via FileSync | Alt+Click: add cursor | "
                           "Ctrl+Alt+Up/Down: column cursor | Escape: clear"));
    }

private slots:
    void syncCycle() {
        m_paneA->poll();
        m_paneB->poll();

        // Write presence and ephemeral for both panes
        m_paneA->writePresence();
        m_paneB->writePresence();
        m_paneA->writeEphemeral(m_ephemeralSeq);
        m_paneB->writeEphemeral(m_ephemeralSeq);
        ++m_ephemeralSeq;

        // Read remote ephemerals and apply cursor overlays
        syncEphemeralCursors(m_paneA, m_paneB);
        syncEphemeralCursors(m_paneB, m_paneA);

        // Update participant list
        updateParticipants();
    }

private:
    void syncEphemeralCursors(EditorPane *local, EditorPane *remote) {
        auto remoteEphemerals = local->presenceManager().read_remote_ephemerals();
        for (const auto &[replicaId, es] : remoteEphemerals) {
            // Look up the identity for this remote replica via presence
            auto remotePresences = local->presenceManager().read_remote_presences();
            for (const auto &[presReplicaId, presence] : remotePresences) {
                if (presReplicaId == replicaId) {
                    auto maybeId = m_projector.read(presence.identity_id);
                    if (maybeId) {
                        local->applyRemoteEphemeral(es, *maybeId);
                    }
                    break;
                }
            }
        }
    }

    void updateParticipants() {
        auto identities = m_projector.read_all();

        // Gather presences from both panes (they share the same folder,
        // so either will return the same set of remote presences plus we
        // need to include the local ones).
        std::vector<Presence> presences;

        // Add local presences by reading what each pane last wrote.
        // The simplest approach: read ALL replicas' presence from one
        // pane's manager (they share the same folder). We read remote
        // presences from pane A, which gives us B's presence, then add
        // A's own presence manually.
        auto remoteFromA = m_paneA->presenceManager().read_remote_presences();
        for (const auto &[rid, p] : remoteFromA) {
            presences.push_back(p);
        }
        // Build A's own presence
        Presence pA;
        pA.replica_id = m_paneA->replicaName();
        pA.identity_id = m_paneA->identity().identity_id;
        pA.active = true;
        pA.last_heartbeat = now_iso8601();
        presences.push_back(pA);

        m_participantList->updateParticipants(identities, presences);
    }

    EditorPane *m_paneA;
    EditorPane *m_paneB;
    ParticipantListWidget *m_participantList;
    IdentityProjector m_projector;
    uint64_t m_ephemeralSeq = 0;
};

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    // Shared CRDT folder
    QTemporaryDir tmpDir;
    tmpDir.setAutoRemove(true);
    std::filesystem::path sharedFolder = tmpDir.path().toStdString();

    // Generate identities in temp directories (not ~/.config/collabtext/)
    QTemporaryDir aliceConfigDir;
    aliceConfigDir.setAutoRemove(true);
    IdentityStore aliceStore(aliceConfigDir.path().toStdString());
    Identity aliceId = aliceStore.generate("Alice");
    aliceId.color = "#4165E1"; // Royal Blue
    aliceStore.save(aliceId);

    QTemporaryDir bobConfigDir;
    bobConfigDir.setAutoRemove(true);
    IdentityStore bobStore(bobConfigDir.path().toStdString());
    Identity bobId = bobStore.generate("Bob");
    bobId.color = "#DC143C"; // Crimson
    bobStore.save(bobId);

    MainWindow window(sharedFolder, aliceId, bobId);
    window.show();
    return app.exec();
}

#include "main.moc"
