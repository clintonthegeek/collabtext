#include <QApplication>
#include <QDateTime>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QPushButton>
#include <QRandomGenerator>
#include <QSplitter>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTimer>
#include <QVBoxLayout>
#include <QTextCursor>
#include <QPlainTextDocumentLayout>

#include <chrono>
#include <ctime>
#include <optional>

#include "crdt/Anchor.h"
#include "crdt/Buffer.h"
#include "crdt/ChatMessage.h"
#include "crdt/Comment.h"
#include "crdt/FileSync.h"
#include "crdt/StreamSync.h"
#include "ui/ChatPanelWidget.h"
#include "ui/CollabPlainTextEdit.h"
#include "ui/CommentsPanelWidget.h"
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
        m_followBtn = new QPushButton(QStringLiteral("Follow"), this);
        m_followBtn->setCheckable(true);
        auto *gremlinBtn = new QPushButton(QStringLiteral("Gremlin: OFF"), this);
        gremlinBtn->setCheckable(true);
        auto *bottomRow = new QHBoxLayout;
        bottomRow->addWidget(m_statusLabel);
        bottomRow->addStretch();
        bottomRow->addWidget(m_followBtn);
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
        connect(m_followBtn, &QPushButton::toggled, this, [this](bool on) {
            m_following = on;
            m_followBtn->setText(on
                ? QStringLiteral("Following %1").arg(m_followTargetName)
                : QStringLiteral("Follow %1").arg(m_followTargetName));
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

        // Undo/redo: routed from the editor widget. CollabPlainTextEdit
        // intercepts the platform's standard undo/redo shortcuts (which
        // QPlainTextEdit would otherwise swallow internally) and emits
        // these signals.
        connect(m_edit, &CollabPlainTextEdit::undoRequested,
                this, &EditorPane::undoLocal);
        connect(m_edit, &CollabPlainTextEdit::redoRequested,
                this, &EditorPane::redoLocal);
        connect(m_edit, &CollabPlainTextEdit::viewportScrolled,
                this, &EditorPane::onViewportScrolled);

        m_sync.start();
    }

    CollabPlainTextEdit *editor() const { return m_edit; }
    Buffer &buffer() { return m_buffer; }
    const Identity &identity() const { return m_identity; }
    PresenceManager &presenceManager() { return m_presence; }
    const std::string &replicaName() const { return m_replicaName; }

    void setFollowTargetName(const QString &name) {
        m_followTargetName = name;
        m_followBtn->setText(QStringLiteral("Follow %1").arg(name));
    }

    /// Run one sync cycle. Saves version before polling, then applies
    /// remote edits surgically to the QTextDocument via edits_since().
    void poll() {
        checkDivergence("before poll");
        Global before = m_buffer.version();
        size_t applied = m_sync.poll();
        if (applied > 0) {
            auto edits = m_buffer.edits_since(before);
            applyEditsPreservingScroll(edits);
            // Remote edits break any in-progress local coalescing run:
            // the user's tracked Qt cursor position may have shifted, and
            // their next keystroke is conceptually a new action even if
            // it lands at the same coordinates.
            m_lastEditKind = EditKind::None;
            checkDivergence("after poll");
        }
        checkDivergence("end of poll");
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

    /// Write ephemeral.json with cursor and viewport anchors.
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

        // Viewport anchors (populated by applyEditsPreservingScroll and
        // onViewportScrolled). nullopt before the first scroll event.
        es.viewport_top = m_viewportTopAnchor;
        es.viewport_bottom = m_viewportBottomAnchor;

        m_presence.write_ephemeral(es);
    }

    /// Apply a remote EphemeralState, resolving anchors and setting remote cursors.
    /// If follow-mode is active, also scroll to the remote user's viewport.
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

        // Follow mode: scroll to the remote user's viewport position.
        if (m_following && es.viewport_top) {
            uint32_t remoteTopByte = m_buffer.resolve_anchor(*es.viewport_top);
            m_followScrolling = true;
            m_edit->scrollByteOffsetToTop(remoteTopByte, /*keepCursorVisible=*/false);
            m_followScrolling = false;
        }
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

        // Pre-edit divergence check: buffer text should match Qt text BEFORE this edit.
        // Qt doc is already modified, so reconstruct the old Qt text from the change.
        // Compare Qt character counts (not byte counts — the buffer stores UTF-8
        // and multi-byte characters would make a byte comparison spuriously fail).
        QString qtNow = m_qtDoc->toPlainText();
        auto expectedQtOldLen = qtNow.length() - charsAdded + charsRemoved;
        if (qBufText.length() != expectedQtOldLen) {
            qWarning("[%s] PRE-EDIT MISMATCH: bufChars=%lld expectedOldQtLen=%lld "
                     "(qtNow=%lld - added=%d + removed=%d)",
                     m_replicaName.c_str(), static_cast<long long>(qBufText.length()),
                     static_cast<long long>(expectedQtOldLen),
                     static_cast<long long>(qtNow.length()), charsAdded, charsRemoved);
        }

        uint32_t byteStart = qBufText.left(position).toUtf8().size();
        uint32_t byteEnd = byteStart;
        if (charsRemoved > 0) {
            byteEnd = qBufText.left(position + charsRemoved).toUtf8().size();
        }

        QString insertedQt;
        std::string inserted;
        if (charsAdded > 0) {
            // Extract the inserted text from the already-fetched toPlainText()
            // rather than constructing a QTextCursor. During contentsChange
            // the document may be in an inconsistent state, and
            // QTextCursor::setPosition can fail for large insertions (e.g.
            // paste), leaving selectedText() empty and the buffer out of sync.
            insertedQt = qtNow.mid(position, charsAdded);
            inserted = insertedQt.toStdString();
        }

        // ---- Coalescing decision (made BEFORE applying the edit) ----
        // We classify this edit, then check whether it can be merged with
        // the previous one to give the user word-level (rather than
        // character-level) Ctrl+Z granularity.
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
                // Continuous typing — same caret position as the end of
                // the previous insert, and the previous run was still
                // mid-word.
                shouldCoalesce = true;
            } else if (thisKind == EditKind::Backspace
                       && position == m_lastEditEndQt - 1) {
                // Continuous backspace — each press deletes the char one
                // position to the left of the previous deletion target.
                shouldCoalesce = true;
            }
        }

        auto op = m_buffer.apply_local_edit({{byteStart, byteEnd}}, {inserted});
        m_sync.push_local_op(op);

        if (shouldCoalesce)
            m_buffer.coalesce_last_undo();

        // ---- Update tracking for the next call ----
        m_lastEditTimeMs = nowMs;
        m_lastUndoDepth = m_buffer.undo_depth();
        m_lastEditKind = thisKind;
        if (thisKind == EditKind::Insert) {
            m_lastEditEndQt = position + charsAdded;
            // A whitespace/newline character joins the current undo group
            // but blocks the NEXT keystroke from joining it.
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

        checkDivergence("after local edit");
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
        // that the next writeEphemeral() broadcasts this position. Without
        // this, Bob's cursor would appear stuck in both panes while the
        // gremlin is editing.
        QTextCursor c = m_edit->textCursor();
        c.setPosition(byteOffsetToQtPos(newCursorByte));
        m_edit->setTextCursor(c);
    }

    void undoLocal() {
        Global before = m_buffer.version();
        auto op = m_buffer.undo();
        if (!op) {
            qDebug("[%s] undo: nothing to undo", m_replicaName.c_str());
            return;
        }
        m_sync.push_local_op(*op);
        applyEditsPreservingScroll(m_buffer.edits_since(before));
        // Hard boundary: subsequent typing must not coalesce across an
        // undo press, even if the cursor returns to the same place.
        m_lastEditKind = EditKind::None;
        m_lastUndoDepth = m_buffer.undo_depth();
        checkDivergence("after undo");
    }

    void redoLocal() {
        Global before = m_buffer.version();
        auto op = m_buffer.redo();
        if (!op) {
            qDebug("[%s] redo: nothing to redo", m_replicaName.c_str());
            return;
        }
        m_sync.push_local_op(*op);
        applyEditsPreservingScroll(m_buffer.edits_since(before));
        m_lastEditKind = EditKind::None;
        m_lastUndoDepth = m_buffer.undo_depth();
        checkDivergence("after redo");
    }

    /// Refresh cached viewport anchors after a local scroll event.
    /// Called from the CollabPlainTextEdit::viewportScrolled signal.
    void onViewportScrolled() {
        uint32_t topByteOff = m_edit->topVisibleByteOffset();
        uint32_t bottomByteOff = m_edit->bottomVisibleByteOffset();
        m_viewportTopAnchor = m_buffer.anchor_at(topByteOff, Bias::Left);
        m_viewportBottomAnchor = m_buffer.anchor_at(bottomByteOff, Bias::Left);

        // Manual scroll disables follow mode. The guard prevents
        // follow-driven scrolls (from applyRemoteEphemeral) from
        // triggering this — those set m_followScrolling = true.
        if (m_following && !m_followScrolling) {
            m_followBtn->setChecked(false);
        }
    }

private:
    void checkDivergence(const char *context) {
        std::string bufText = m_buffer.text();
        QString qtText = m_qtDoc->toPlainText();
        std::string qtUtf8 = qtText.toStdString();
        if (bufText != qtUtf8) {
            qWarning("DIVERGENCE [%s] in %s! Buffer(%zu bytes) != Qt(%zu bytes)",
                     m_replicaName.c_str(), context,
                     bufText.size(), qtUtf8.size());
            // Show first difference
            size_t minLen = std::min(bufText.size(), qtUtf8.size());
            for (size_t i = 0; i < minLen; ++i) {
                if (bufText[i] != qtUtf8[i]) {
                    qWarning("  First diff at byte %zu: buffer=0x%02x qt=0x%02x",
                             i, (unsigned char)bufText[i], (unsigned char)qtUtf8[i]);
                    qWarning("  Buffer around diff: ...%.40s...",
                             bufText.substr(i > 20 ? i - 20 : 0, 60).c_str());
                    qWarning("  Qt around diff:     ...%.40s...",
                             qtUtf8.substr(i > 20 ? i - 20 : 0, 60).c_str());
                    break;
                }
            }
            if (bufText.size() != qtUtf8.size() && minLen == std::min(bufText.size(), qtUtf8.size())) {
                qWarning("  Lengths differ: buffer=%zu qt=%zu", bufText.size(), qtUtf8.size());
            }
        }
    }

    /// Apply remote edits while preserving the local viewport scroll
    /// and the local cursor visibility. Capture a CRDT anchor at the
    /// top-visible byte offset before mutating, restore it after.
    /// This is the public entry point for any remote-edit path; the
    /// private applyEditsToQt is the raw UTF-16 mutation helper.
    void applyEditsPreservingScroll(const std::vector<TextEdit> &edits) {
        if (edits.empty()) return;

        // Capture viewport anchors before the tree mutates.
        uint32_t topByteOff = m_edit->topVisibleByteOffset();
        Anchor topAnchor = m_buffer.anchor_at(topByteOff, Bias::Left);

        uint32_t bottomByteOff = m_edit->bottomVisibleByteOffset();
        Anchor bottomAnchor = m_buffer.anchor_at(bottomByteOff, Bias::Left);

        applyEditsToQt(edits);

        // Restore scroll.
        uint32_t newTopByteOff = m_buffer.resolve_anchor(topAnchor);
        m_edit->scrollByteOffsetToTop(newTopByteOff,
                                      /*keepCursorVisible=*/true);

        // Cache the anchors for the next EphemeralState flush.
        m_viewportTopAnchor = topAnchor;
        m_viewportBottomAnchor = bottomAnchor;
    }

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
    QPushButton *m_followBtn;
    QTimer *m_gremlinTimer = nullptr;

    // -------- Follow mode --------
    QString m_followTargetName;
    bool m_following = false;
    bool m_followScrolling = false;  // guard: true during programmatic follow scroll

    // -------- Undo coalescing state --------
    // Used by onContentsChange to decide whether the new edit should be
    // grouped with the previous one (consecutive typing within a word, or
    // a run of backspaces). All "Qt position" values are character offsets
    // in the QTextDocument's coordinate space.
    enum class EditKind { None, Insert, Backspace, Other };
    static constexpr qint64 kCoalesceWindowMs = 500;

    EditKind m_lastEditKind = EditKind::None;
    qint64 m_lastEditTimeMs = 0;
    int m_lastEditEndQt = -1;            // Insert: post-insertion pos. Backspace: cursor pos after delete.
    bool m_lastInsertEndsOnWordBoundary = false;
    size_t m_lastUndoDepth = 0;          // Buffer's undo_depth() right after the last user edit

    // -------- Cached viewport anchors for ephemeral broadcast --------
    // Refreshed whenever a remote edit cycle runs (via
    // applyEditsPreservingScroll) or the local user scrolls the widget
    // (via onViewportScrolled). writeEphemeral reads these into
    // EphemeralState.viewport_top / viewport_bottom on each flush.
    std::optional<Anchor> m_viewportTopAnchor;
    std::optional<Anchor> m_viewportBottomAnchor;
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    ~MainWindow() override { delete m_streamSync; }

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
        m_paneA->setFollowTargetName(QString::fromStdString(bobId.display_name));
        m_paneB->setFollowTargetName(QString::fromStdString(aliceId.display_name));
        layout->addWidget(m_paneA, 1);
        layout->addWidget(m_paneB, 1);

        // Right sidebar: participant list + chat panel in a vertical splitter
        auto *sidebar = new QSplitter(Qt::Vertical, central);
        sidebar->setFixedWidth(250);

        m_participantList = new ParticipantListWidget(sidebar);
        sidebar->addWidget(m_participantList);

        m_chatPanel = new ChatPanelWidget(sidebar);
        sidebar->addWidget(m_chatPanel);

        m_commentsPanel = new CommentsPanelWidget(sidebar);
        sidebar->addWidget(m_commentsPanel);

        sidebar->setStretchFactor(0, 1);  // participants
        sidebar->setStretchFactor(1, 2);  // chat
        sidebar->setStretchFactor(2, 2);  // comments

        layout->addWidget(sidebar);

        // Stream sync for chat and comments
        m_streamSync = new StreamSync(sharedFolder, "main");
        m_streamSync->register_stream("chat", StreamSync::StreamType::AppendOnly);
        m_streamSync->register_stream("comments", StreamSync::StreamType::AnchorKeyed);
        m_streamSync->start();

        connect(m_chatPanel, &ChatPanelWidget::messageSent,
                this, &MainWindow::onChatMessageSent);

        connect(m_commentsPanel, &CommentsPanelWidget::addCommentRequested,
                this, &MainWindow::onAddComment);

        connect(m_commentsPanel, &CommentsPanelWidget::commentClicked,
                this, [this](const QString &commentId) {
            auto entries = m_streamSync->entries("comments");
            for (const auto &entry : entries) {
                if (entry.id == commentId.toStdString()) {
                    auto c = comment_from_entry(entry);
                    if (!c) break;
                    uint32_t byteOff = m_paneA->buffer().resolve_anchor(c->range_start);
                    EditorPane *pane = m_paneB->editor()->hasFocus() ? m_paneB : m_paneA;
                    pane->editor()->scrollByteOffsetToTop(byteOff, false);
                    break;
                }
            }
        });

        connect(m_commentsPanel, &CommentsPanelWidget::resolveRequested,
                this, [this](const QString &id) { setCommentResolved(id, true); });

        connect(m_commentsPanel, &CommentsPanelWidget::unresolveRequested,
                this, [this](const QString &id) { setCommentResolved(id, false); });

        connect(m_commentsPanel, &CommentsPanelWidget::deleteRequested,
                this, &MainWindow::deleteComment);

        connect(m_chatPanel, &ChatPanelWidget::anchorClicked,
                this, [this](int line) {
            std::string text = m_paneA->buffer().text();
            uint32_t byteOff = 0;
            int currentLine = 1;
            for (size_t i = 0; i < text.size() && currentLine < line; ++i) {
                if (text[i] == '\n') ++currentLine;
                byteOff = static_cast<uint32_t>(i + 1);
            }
            EditorPane *pane = m_paneB->editor()->hasFocus() ? m_paneB : m_paneA;
            pane->editor()->scrollByteOffsetToTop(byteOff, /*keepCursorVisible=*/false);
        });

        setCentralWidget(central);

        auto *syncTimer = new QTimer(this);
        connect(syncTimer, &QTimer::timeout, this, &MainWindow::syncCycle);
        syncTimer->start(100);

        statusBar()->showMessage(
            QStringLiteral("Full-stack sync via FileSync | Alt+Click: add cursor | "
                           "Ctrl+Alt+Up/Down: column cursor | Escape: clear"));
    }

private slots:
    void onAddComment(const QString &body) {
        EditorPane *pane = m_paneB->editor()->hasFocus() ? m_paneB : m_paneA;
        auto cursor = pane->editor()->textCursor();
        if (!cursor.hasSelection()) return;

        int qtStart = cursor.selectionStart();
        int qtEnd = cursor.selectionEnd();
        uint32_t byteStart = pane->qtPosToByteOffset(qtStart);
        uint32_t byteEnd = pane->qtPosToByteOffset(qtEnd);

        const auto &id = pane->identity();

        ++m_commentSeq;
        Comment c;
        c.id = "c-" + std::to_string(m_commentSeq);
        c.replica_id = 0;
        c.seq = m_commentSeq;
        c.timestamp = now_iso8601();
        c.author = id.identity_id;
        c.author_name = id.display_name;
        c.body = body.toStdString();
        c.range_start = pane->buffer().anchor_at(byteStart, Bias::Left);
        c.range_end = pane->buffer().anchor_at(byteEnd, Bias::Right);

        m_streamSync->push("comments", comment_to_entry(c));
    }

    void setCommentResolved(const QString &id, bool resolved) {
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

    void deleteComment(const QString &id) {
        StreamEntry tomb;
        tomb.id        = id.toStdString();
        tomb.timestamp = now_iso8601();
        tomb.tombstone = true;
        m_streamSync->push("comments", tomb);
    }

    void onChatMessageSent(const QString &body) {
        // Determine author by which editor has focus
        const auto &id = m_paneB->editor()->hasFocus()
            ? m_paneB->identity() : m_paneA->identity();

        ++m_chatSeq;
        ChatMessage msg;
        msg.replica_id = 0;
        msg.seq = m_chatSeq;
        msg.id = "0-" + std::to_string(m_chatSeq);
        msg.timestamp = now_iso8601();
        msg.author = id.identity_id;
        msg.author_name = id.display_name;
        msg.body = body.toStdString();

        // Capture document anchor from the active editor's cursor
        EditorPane *activePane = m_paneB->editor()->hasFocus() ? m_paneB : m_paneA;
        auto cursor = activePane->editor()->textCursor();
        int qtPos = cursor.hasSelection() ? cursor.selectionStart() : cursor.position();
        uint32_t byteOff = activePane->qtPosToByteOffset(qtPos);
        msg.anchor = activePane->buffer().anchor_at(byteOff, Bias::Left);

        m_streamSync->push("chat", chat_message_to_entry(msg));
    }

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

        // Sync chat
        m_streamSync->poll();
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
                uint32_t byteOff = m_paneA->buffer().resolve_anchor(*msg->anchor);
                std::string text = m_paneA->buffer().text();
                int line = 1;
                for (uint32_t i = 0; i < byteOff && i < text.size(); ++i) {
                    if (text[i] == '\n') ++line;
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

        // Sync comments
        auto commentEntries = m_streamSync->entries("comments");
        QList<std::tuple<uint32_t, uint32_t, QColor>> highlightsA, highlightsB;
        QList<CommentDisplayInfo> commentDisplayList;

        for (const auto &entry : commentEntries) {
            auto c = comment_from_entry(entry);
            if (!c) continue;

            uint32_t startByte = m_paneA->buffer().resolve_anchor(c->range_start);
            uint32_t endByte = m_paneA->buffer().resolve_anchor(c->range_end);

            QColor color(Qt::yellow);
            auto maybeId = m_projector.read(c->author);
            if (maybeId)
                color = QColor(QString::fromStdString(maybeId->color));

            if (!c->resolved) {
                highlightsA.append({startByte, endByte, color});
                highlightsB.append({startByte, endByte, color});
            }

            std::string text = m_paneA->buffer().text();
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

        m_paneA->editor()->setCommentHighlights(highlightsA);
        m_paneB->editor()->setCommentHighlights(highlightsB);
        m_commentsPanel->setComments(commentDisplayList);
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
    ChatPanelWidget *m_chatPanel;
    CommentsPanelWidget *m_commentsPanel;
    StreamSync *m_streamSync = nullptr;

    uint64_t m_chatSeq = 0;
    size_t m_lastChatCount = 0;
    IdentityProjector m_projector;
    uint64_t m_ephemeralSeq = 0;
    uint64_t m_commentSeq = 0;
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
