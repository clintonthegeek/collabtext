#include "CollabPane.h"

#include "crdt/ChatMessage.h"
#include "crdt/Comment.h"
#include "crdt/SeedOp.h"
#include "ui/ChatPanelWidget.h"
#include "ui/CollabPlainTextEdit.h"
#include "ui/CommentsPanelWidget.h"
#include "ui/MultiCursorController.h"
#include "ui/ParticipantListWidget.h"

#include <QDateTime>
#include <QDir>
#include <QHBoxLayout>
#include <QPlainTextDocumentLayout>
#include <QPushButton>
#include <QRandomGenerator>
#include <QSplitter>
#include <QStandardPaths>
#include <QTextCursor>
#include <QTextDocument>
#include <QTimer>
#include <QVBoxLayout>

#include <chrono>
#include <ctime>
#include <cstring>
#include <fstream>
#include <set>

using namespace CollabText;
using namespace CollabText::Crdt;
using namespace CollabText::Identity;
using namespace CollabText::Ui;

namespace CollabEdit {

namespace {

constexpr qint64 kPresenceIntervalMs       = 5000;
constexpr qint64 kEphemeralMinIntervalMs   = 250;
constexpr qint64 kEphemeralMaxIntervalMs   = 5000;
constexpr qint64 kCoalesceWindowMs         = 500;

std::string now_iso8601() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

uint16_t loadOrAssignReplicaId(const std::filesystem::path &sidecar,
                               const std::string &doc_id) {
    namespace fs = std::filesystem;

    QString configRoot = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QDir().mkpath(configRoot + "/replica-state");
    auto rid_path = fs::path(configRoot.toStdString())
                  / "replica-state"
                  / (doc_id + ".replica_id");

    if (fs::exists(rid_path)) {
        std::ifstream f(rid_path);
        unsigned int x = 0;
        f >> x;
        if (x >= 1 && x <= 65535) return static_cast<uint16_t>(x);
    }

    std::set<uint16_t> taken;
    auto replicas_dir = sidecar / "replicas";
    if (fs::exists(replicas_dir)) {
        for (const auto &entry : fs::directory_iterator(replicas_dir)) {
            if (!entry.is_directory()) continue;
            auto pres = entry.path() / "presence.json";
            if (!fs::exists(pres)) continue;
            std::ifstream pf(pres);
            std::string content((std::istreambuf_iterator<char>(pf)),
                                std::istreambuf_iterator<char>());
            // version_summary is sparse: {"0":8,"197":60}
            auto vpos = content.find("\"version_summary\":{");
            if (vpos == std::string::npos) continue;
            auto end = content.find('}', vpos);
            if (end == std::string::npos) continue;
            std::string body = content.substr(vpos + 19, end - vpos - 19);
            // body looks like: "0":8,"197":60
            size_t p = 0;
            while (p < body.size()) {
                size_t qstart = body.find('"', p);
                if (qstart == std::string::npos) break;
                size_t qend = body.find('"', qstart + 1);
                if (qend == std::string::npos) break;
                try {
                    unsigned long idx = std::stoul(body.substr(qstart + 1, qend - qstart - 1));
                    if (idx >= 1 && idx <= 65535) taken.insert(static_cast<uint16_t>(idx));
                } catch (...) {}
                p = body.find(',', qend);
                if (p == std::string::npos) break;
                ++p;
            }
        }
    }

    uint16_t chosen = 0;
    auto *rng = QRandomGenerator::global();
    for (int attempt = 0; attempt < 500; ++attempt) {
        uint16_t candidate = static_cast<uint16_t>(rng->bounded(250) + 1);
        if (taken.count(candidate) == 0) { chosen = candidate; break; }
    }
    if (chosen == 0) chosen = 1;

    std::ofstream out(rid_path);
    out << chosen;
    return chosen;
}

} // namespace

CollabPane::CollabPane(CollabText::Identity::Identity identity,
                       std::string replica_name,
                       std::filesystem::path sidecar_dir,
                       std::string doc_id,
                       const std::string &seed_text,
                       QWidget *parent)
    : QWidget(parent)
    , m_identity(std::move(identity))
    , m_replicaName(std::move(replica_name))
    , m_buffer(loadOrAssignReplicaId(sidecar_dir, doc_id))
    , m_sync(m_buffer, sidecar_dir, m_replicaName)
    , m_presence(sidecar_dir, m_replicaName, m_identity.identity_id)
    , m_projector(sidecar_dir)
    , m_streamSync(std::make_unique<StreamSync>(sidecar_dir, m_replicaName))
    , m_sessionStarted(now_iso8601())
{
    // Project our identity so other peers can resolve our display_name
    // and color. Idempotent: re-projects on each launch (cheap).
    m_projector.project(m_identity);

    m_streamSync->register_stream("chat", StreamSync::StreamType::AppendOnly);
    m_streamSync->register_stream("comments", StreamSync::StreamType::AnchorKeyed);
    m_streamSync->start();

    auto *root = new QHBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);

    // Editor + bottom-row buttons in a vertical column on the left.
    auto *editorCol = new QWidget(this);
    auto *editorColLayout = new QVBoxLayout(editorCol);
    editorColLayout->setContentsMargins(0, 0, 0, 0);

    m_edit = new CollabPlainTextEdit(editorCol);
    m_qtDoc = new QTextDocument(this);
    m_qtDoc->setDocumentLayout(new QPlainTextDocumentLayout(m_qtDoc));
    m_qtDoc->setUndoRedoEnabled(false);
    m_edit->setDocument(m_qtDoc);
    editorColLayout->addWidget(m_edit, 1);

    auto *btnRow = new QHBoxLayout;
    btnRow->setContentsMargins(4, 2, 4, 2);
    btnRow->addStretch();
    m_followBtn = new QPushButton(tr("Follow"), editorCol);
    m_followBtn->setCheckable(true);
    btnRow->addWidget(m_followBtn);
    m_gremlinBtn = new QPushButton(tr("Gremlin: OFF"), editorCol);
    m_gremlinBtn->setCheckable(true);
    btnRow->addWidget(m_gremlinBtn);
    editorColLayout->addLayout(btnRow);

    root->addWidget(editorCol, 1);

    // Sidebar: participants / chat / comments stacked vertically.
    auto *sidebar = new QSplitter(Qt::Vertical, this);
    sidebar->setFixedWidth(260);
    m_participantList = new ParticipantListWidget(sidebar);
    m_chatPanel       = new ChatPanelWidget(sidebar);
    m_commentsPanel   = new CommentsPanelWidget(sidebar);
    sidebar->addWidget(m_participantList);
    sidebar->addWidget(m_chatPanel);
    sidebar->addWidget(m_commentsPanel);
    sidebar->setStretchFactor(0, 1);
    sidebar->setStretchFactor(1, 2);
    sidebar->setStretchFactor(2, 2);
    root->addWidget(sidebar);

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

    // Undo/redo: CollabPlainTextEdit intercepts the platform's standard
    // shortcuts and emits these signals — route them through the CRDT
    // so undos produce real ops that propagate to remote peers.
    connect(m_edit, &CollabPlainTextEdit::undoRequested,
            this, &CollabPane::undoLocal);
    connect(m_edit, &CollabPlainTextEdit::redoRequested,
            this, &CollabPane::redoLocal);
    connect(m_edit, &CollabPlainTextEdit::viewportScrolled,
            this, &CollabPane::onViewportScrolled);

    // Buttons.
    connect(m_followBtn, &QPushButton::toggled, this, [this](bool on) {
        m_following = on;
        m_followBtn->setText(on
            ? (m_followTargetName.isEmpty()
                  ? tr("Following")
                  : tr("Following %1").arg(m_followTargetName))
            : tr("Follow"));
    });

    m_gremlinTimer = new QTimer(this);
    m_gremlinTimer->setInterval(80);
    connect(m_gremlinTimer, &QTimer::timeout, this, &CollabPane::gremlinTick);
    connect(m_gremlinBtn, &QPushButton::toggled, this, [this](bool on) {
        if (on) {
            m_gremlinTimer->start();
            m_gremlinBtn->setText(tr("Gremlin: ON"));
        } else {
            m_gremlinTimer->stop();
            m_gremlinBtn->setText(tr("Gremlin: OFF"));
        }
    });

    // Chat / comments wiring.
    connect(m_chatPanel, &ChatPanelWidget::messageSent,
            this, &CollabPane::onChatMessageSent);
    connect(m_chatPanel, &ChatPanelWidget::anchorClicked,
            this, &CollabPane::onChatAnchorClicked);

    connect(m_commentsPanel, &CommentsPanelWidget::addCommentRequested,
            this, &CollabPane::onAddComment);
    connect(m_commentsPanel, &CommentsPanelWidget::commentClicked,
            this, &CollabPane::onCommentClicked);
    connect(m_commentsPanel, &CommentsPanelWidget::resolveRequested,
            this, [this](const QString &id) { setCommentResolved(id, true); });
    connect(m_commentsPanel, &CommentsPanelWidget::unresolveRequested,
            this, [this](const QString &id) { setCommentResolved(id, false); });
    connect(m_commentsPanel, &CommentsPanelWidget::deleteRequested,
            this, &CollabPane::deleteComment);

    m_syncTimer = new QTimer(this);
    m_syncTimer->setInterval(250);
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
    if (m_gremlinTimer) m_gremlinTimer->stop();
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

    QString insertedQt;
    std::string inserted;
    if (charsAdded > 0) {
        insertedQt = qtNow.mid(position, charsAdded);
        inserted = insertedQt.toStdString();
    }

    // Coalescing decision (made BEFORE applying the edit).
    EditKind thisKind = EditKind::Other;
    if (charsRemoved == 0 && charsAdded == 1) {
        thisKind = EditKind::Insert;
    } else if (charsRemoved == 1 && charsAdded == 0) {
        thisKind = EditKind::Backspace;
    }

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const bool inWindow = (nowMs - m_lastEditTimeMs) < kCoalesceWindowMs;
    // Continuity guard: if anything else (gremlin, programmatic edit)
    // pushed an entry onto the stack between the previous user edit
    // and this one, we MUST NOT coalesce — the previous undo entry
    // is no longer ours.
    const bool stackContinuous = (m_buffer.undo_depth() == m_lastUndoDepth);

    bool shouldCoalesce = false;
    if (inWindow && stackContinuous && m_lastEditKind == thisKind) {
        if (thisKind == EditKind::Insert
            && !m_lastInsertEndsOnWordBoundary
            && position == m_lastEditEndQt) {
            shouldCoalesce = true;
        } else if (thisKind == EditKind::Backspace
                   && position == m_lastEditEndQt - 1) {
            shouldCoalesce = true;
        }
    }

    auto op = m_buffer.apply_local_edit({{byteStart, byteEnd}}, {inserted});
    m_sync.push_local_op(op);

    if (shouldCoalesce)
        m_buffer.coalesce_last_undo();

    m_lastEditTimeMs = nowMs;
    m_lastUndoDepth = m_buffer.undo_depth();
    m_lastEditKind = thisKind;
    if (thisKind == EditKind::Insert) {
        m_lastEditEndQt = position + charsAdded;
        QChar lastCh = insertedQt.isEmpty() ? QChar()
                                             : insertedQt.at(insertedQt.size() - 1);
        m_lastInsertEndsOnWordBoundary = lastCh.isSpace();
    } else if (thisKind == EditKind::Backspace) {
        m_lastEditEndQt = position;
        m_lastInsertEndsOnWordBoundary = false;
    } else {
        m_lastEditEndQt = -1;
        m_lastInsertEndsOnWordBoundary = false;
    }
}

void CollabPane::onViewportScrolled() {
    uint32_t topByteOff = m_edit->topVisibleByteOffset();
    uint32_t bottomByteOff = m_edit->bottomVisibleByteOffset();
    m_viewportTopAnchor = m_buffer.anchor_at(topByteOff, Bias::Left);
    m_viewportBottomAnchor = m_buffer.anchor_at(bottomByteOff, Bias::Left);

    // Manual scroll disables follow mode (unless this scroll was itself
    // driven by follow-mode applying a remote viewport).
    if (m_following && !m_followScrolling) {
        m_followBtn->setChecked(false);
    }
}

void CollabPane::undoLocal() {
    Global before = m_buffer.version();
    auto op = m_buffer.undo();
    if (!op) return;
    m_sync.push_local_op(*op);
    applyEditsPreservingScroll(m_buffer.edits_since(before));
    m_lastEditKind = EditKind::None;
    m_lastUndoDepth = m_buffer.undo_depth();
}

void CollabPane::redoLocal() {
    Global before = m_buffer.version();
    auto op = m_buffer.redo();
    if (!op) return;
    m_sync.push_local_op(*op);
    applyEditsPreservingScroll(m_buffer.edits_since(before));
    m_lastEditKind = EditKind::None;
    m_lastUndoDepth = m_buffer.undo_depth();
}

void CollabPane::gremlinTick() {
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
    uint32_t newCursorByte = 0;

    if (roll == 0 && docLen > 10) {
        uint32_t delLen = qMin(static_cast<uint32_t>(rng->bounded(3, 9)), docLen);
        uint32_t pos = rng->bounded(docLen - delLen + 1);
        op = m_buffer.apply_local_edit({{pos, pos + delLen}}, {""});
        newCursorByte = pos;
    } else if (roll < 3 && docLen > 0) {
        uint32_t pos = rng->bounded(docLen + 1);
        op = m_buffer.apply_local_edit({{pos, pos}}, {"\n"});
        newCursorByte = pos + 1;
    } else {
        uint32_t pos = docLen;
        if (rng->bounded(5) == 0 && docLen > 0)
            pos = rng->bounded(docLen + 1);
        const char *word = lorem[rng->bounded(nwords)];
        op = m_buffer.apply_local_edit({{pos, pos}}, {word});
        newCursorByte = pos + static_cast<uint32_t>(strlen(word));
    }

    m_sync.push_local_op(op);
    applyEditsPreservingScroll(m_buffer.edits_since(before));

    // Move the widget's text cursor to where the gremlin just typed so
    // that the next writeEphemeral() broadcasts this position.
    QTextCursor c = m_edit->textCursor();
    c.setPosition(byteOffsetToQtPos(newCursorByte));
    m_edit->setTextCursor(c);
}

void CollabPane::syncCycle() {
    Global before = m_buffer.version();
    size_t applied = m_sync.poll();
    if (applied > 0) {
        applyEditsPreservingScroll(m_buffer.edits_since(before));
        // Remote edits break any in-progress local coalescing run.
        m_lastEditKind = EditKind::None;
    }

    writePresence();
    writeEphemeral();
    applyRemoteCursorsAndFollow();
    syncStreams();
}

void CollabPane::writePresence() {
    qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (m_lastPresenceMs != 0
        && (nowMs - m_lastPresenceMs) < kPresenceIntervalMs) {
        return;
    }
    Presence p;
    p.replica_id = m_replicaName;
    p.identity_id = m_identity.identity_id;
    p.device_name = m_replicaName;
    p.active = true;
    p.last_heartbeat = now_iso8601();
    p.session_started = m_sessionStarted;
    p.version_summary = m_buffer.version();
    m_presence.write_presence(p);
    m_lastPresenceMs = nowMs;
}

void CollabPane::writeEphemeral() {
    auto cursor = m_edit->textCursor();
    int bytePos = static_cast<int>(qtPosToByteOffset(cursor.position()));
    int byteAnchor = static_cast<int>(qtPosToByteOffset(cursor.anchor()));

    qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    qint64 sinceLast = nowMs - m_lastEphemeralMs;
    bool cursorChanged = (bytePos != m_lastEphemeralBytePos)
                      || (byteAnchor != m_lastEphemeralByteAnchor);
    bool changedAndCooledDown = cursorChanged && sinceLast >= kEphemeralMinIntervalMs;
    bool slowHeartbeat       = !cursorChanged && sinceLast >= kEphemeralMaxIntervalMs;
    if (m_lastEphemeralMs != 0 && !changedAndCooledDown && !slowHeartbeat) {
        return;
    }

    EphemeralState es;
    es.seq = ++m_ephemeralSeq;
    es.timestamp = now_iso8601();
    es.activity = "editing";
    auto posAnchor = m_buffer.anchor_at(static_cast<uint32_t>(bytePos), Bias::Right);
    auto selAnchor = m_buffer.anchor_at(static_cast<uint32_t>(byteAnchor), Bias::Left);
    es.cursors.push_back({selAnchor, posAnchor});
    es.viewport_top = m_viewportTopAnchor;
    es.viewport_bottom = m_viewportBottomAnchor;

    m_presence.write_ephemeral(es);
    m_lastEphemeralMs = nowMs;
    m_lastEphemeralBytePos = bytePos;
    m_lastEphemeralByteAnchor = byteAnchor;
}

void CollabPane::applyRemoteCursorsAndFollow() {
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

    // Remember the first live remote display name as the follow target.
    QString firstRemoteName;
    for (const auto &p : allPresences) {
        if (p.replica_id == m_replicaName) continue;
        if (!PresenceManager::is_live(p)) continue;
        auto id = m_projector.read(p.identity_id);
        if (id) { firstRemoteName = QString::fromStdString(id->display_name); break; }
    }
    if (firstRemoteName != m_followTargetName) {
        m_followTargetName = firstRemoteName;
        if (!m_following) {
            m_followBtn->setText(m_followTargetName.isEmpty()
                                     ? tr("Follow")
                                     : tr("Follow %1").arg(m_followTargetName));
        } else {
            m_followBtn->setText(tr("Following %1").arg(m_followTargetName));
        }
    }

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

        // Follow mode: scroll to the remote user's viewport.
        if (m_following && es.viewport_top) {
            uint32_t remoteTopByte = m_buffer.resolve_anchor(*es.viewport_top);
            m_followScrolling = true;
            m_edit->scrollByteOffsetToTop(remoteTopByte, /*keepCursorVisible=*/false);
            m_followScrolling = false;
        }
    }
    m_edit->multiCursorController()->setRemoteCursors(cursors);

    auto identities = m_projector.read_all();
    m_participantList->updateParticipants(identities, allPresences);
}

void CollabPane::syncStreams() {
    m_streamSync->poll();

    // Chat
    auto chatEntries = m_streamSync->entries("chat");
    size_t chatCount = chatEntries.size();
    for (size_t i = m_lastChatCount; i < chatCount; ++i) {
        auto msg = chat_message_from_entry(chatEntries[i]);
        if (!msg) continue;
        QColor color(Qt::gray);
        auto maybeId = m_projector.read(msg->author);
        if (maybeId)
            color = QColor(QString::fromStdString(maybeId->color));
        int anchorLine = -1;
        if (msg->anchor) {
            uint32_t byteOff = m_buffer.resolve_anchor(*msg->anchor);
            std::string text = m_buffer.text();
            int line = 1;
            for (uint32_t j = 0; j < byteOff && j < text.size(); ++j) {
                if (text[j] == '\n') ++line;
            }
            anchorLine = line;
        }
        m_chatPanel->addMessage(
            QString::fromStdString(msg->author_name),
            QString::fromStdString(msg->body),
            QString::fromStdString(msg->timestamp),
            color,
            anchorLine);
    }
    m_lastChatCount = chatCount;

    // Comments
    auto commentEntries = m_streamSync->entries("comments");
    QList<std::tuple<uint32_t, uint32_t, QColor>> highlights;
    QList<CommentDisplayInfo> commentDisplayList;
    std::string text = m_buffer.text();

    for (const auto &entry : commentEntries) {
        auto c = comment_from_entry(entry);
        if (!c) continue;

        uint32_t startByte = m_buffer.resolve_anchor(c->range_start);
        uint32_t endByte   = m_buffer.resolve_anchor(c->range_end);

        QColor color(Qt::yellow);
        auto maybeId = m_projector.read(c->author);
        if (maybeId)
            color = QColor(QString::fromStdString(maybeId->color));

        if (!c->resolved) {
            highlights.append({startByte, endByte, color});
        }

        std::string snippet;
        if (startByte < text.size()) {
            uint32_t len = std::min(endByte - startByte, uint32_t(40));
            snippet = text.substr(startByte, len);
            if (endByte - startByte > 40) snippet += "...";
        }

        CommentDisplayInfo info;
        info.id = QString::fromStdString(c->id);
        info.authorName = QString::fromStdString(c->author_name);
        info.body = QString::fromStdString(c->body);
        info.contextSnippet = QString::fromStdString(snippet);
        info.authorColor = color;
        info.resolved = c->resolved;
        commentDisplayList.append(info);
    }
    m_edit->setCommentHighlights(highlights);
    m_commentsPanel->setComments(commentDisplayList);
}

void CollabPane::onChatMessageSent(const QString &body) {
    auto cursor = m_edit->textCursor();
    int qtPos = cursor.hasSelection() ? cursor.selectionStart() : cursor.position();
    uint32_t byteOff = qtPosToByteOffset(qtPos);

    ++m_chatSeq;
    ChatMessage msg;
    msg.replica_id = m_buffer.replica_id();
    msg.seq = m_chatSeq;
    msg.id = std::to_string(msg.replica_id) + "-" + std::to_string(m_chatSeq);
    msg.timestamp = now_iso8601();
    msg.author = m_identity.identity_id;
    msg.author_name = m_identity.display_name;
    msg.body = body.toStdString();
    msg.anchor = m_buffer.anchor_at(byteOff, Bias::Left);

    m_streamSync->push("chat", chat_message_to_entry(msg));
}

void CollabPane::onAddComment(const QString &body) {
    auto cursor = m_edit->textCursor();
    if (!cursor.hasSelection()) return;

    int qtStart = cursor.selectionStart();
    int qtEnd   = cursor.selectionEnd();
    uint32_t byteStart = qtPosToByteOffset(qtStart);
    uint32_t byteEnd   = qtPosToByteOffset(qtEnd);

    ++m_commentSeq;
    Comment c;
    c.id = "c-" + std::to_string(m_buffer.replica_id())
         + "-" + std::to_string(m_commentSeq);
    c.replica_id = m_buffer.replica_id();
    c.seq = m_commentSeq;
    c.timestamp = now_iso8601();
    c.author = m_identity.identity_id;
    c.author_name = m_identity.display_name;
    c.body = body.toStdString();
    c.range_start = m_buffer.anchor_at(byteStart, Bias::Left);
    c.range_end   = m_buffer.anchor_at(byteEnd,   Bias::Right);

    m_streamSync->push("comments", comment_to_entry(c));
}

void CollabPane::onCommentClicked(const QString &commentId) {
    auto entries = m_streamSync->entries("comments");
    for (const auto &entry : entries) {
        if (entry.id != commentId.toStdString()) continue;
        auto c = comment_from_entry(entry);
        if (!c) return;
        uint32_t byteOff = m_buffer.resolve_anchor(c->range_start);
        m_edit->scrollByteOffsetToTop(byteOff, /*keepCursorVisible=*/false);
        return;
    }
}

void CollabPane::onChatAnchorClicked(int line) {
    std::string text = m_buffer.text();
    uint32_t byteOff = 0;
    int currentLine = 1;
    for (size_t i = 0; i < text.size() && currentLine < line; ++i) {
        if (text[i] == '\n') ++currentLine;
        byteOff = static_cast<uint32_t>(i + 1);
    }
    m_edit->scrollByteOffsetToTop(byteOff, /*keepCursorVisible=*/false);
}

void CollabPane::setCommentResolved(const QString &id, bool resolved) {
    auto entries = m_streamSync->entries("comments");
    for (const auto &entry : entries) {
        if (entry.id != id.toStdString()) continue;
        auto c = comment_from_entry(entry);
        if (!c) return;
        c->resolved  = resolved;
        c->timestamp = now_iso8601();
        m_streamSync->push("comments", comment_to_entry(*c));
        return;
    }
}

void CollabPane::deleteComment(const QString &id) {
    StreamEntry tomb;
    tomb.id        = id.toStdString();
    tomb.timestamp = now_iso8601();
    tomb.tombstone = true;
    m_streamSync->push("comments", tomb);
}

void CollabPane::applyEditsPreservingScroll(const std::vector<TextEdit> &edits) {
    if (edits.empty()) return;

    uint32_t topByteOff = m_edit->topVisibleByteOffset();
    Anchor topAnchor = m_buffer.anchor_at(topByteOff, Bias::Left);
    uint32_t bottomByteOff = m_edit->bottomVisibleByteOffset();
    Anchor bottomAnchor = m_buffer.anchor_at(bottomByteOff, Bias::Left);

    applyEditsToQt(edits);

    uint32_t newTopByteOff = m_buffer.resolve_anchor(topAnchor);
    m_edit->scrollByteOffsetToTop(newTopByteOff, /*keepCursorVisible=*/true);

    m_viewportTopAnchor = topAnchor;
    m_viewportBottomAnchor = bottomAnchor;
}

void CollabPane::applyEditsToQt(const std::vector<TextEdit> &edits) {
    if (edits.empty()) return;
    m_syncing = true;

    for (auto it = edits.rbegin(); it != edits.rend(); ++it) {
        int qtStart = byteOffsetToQtPos(it->old_start);
        int qtEnd   = byteOffsetToQtPos(it->old_end);
        QTextCursor cursor(m_qtDoc);
        cursor.setPosition(qtStart);
        if (qtEnd > qtStart)
            cursor.setPosition(qtEnd, QTextCursor::KeepAnchor);
        QString replacement = QString::fromUtf8(
            it->new_text.data(), static_cast<int>(it->new_text.size()));
        cursor.insertText(replacement);
    }

    m_syncing = false;
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
