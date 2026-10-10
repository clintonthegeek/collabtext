#pragma once

#include "collabtext/Identity.h"
#include "collabtext/IdentityProjector.h"
#include "collabtext/PresenceManager.h"
#include <collabtext/DocumentSession.h>
#include <collabtext/ActorId.h>
#include "crdt/Anchor.h"
#include "transport/StreamSync.h"

#include <QWidget>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

class QPushButton;
class QTextDocument;
class QTimer;

namespace CollabText::Ui {
class ChatPanelWidget;
class CollabPlainTextEdit;
class CommentsPanelWidget;
class ParticipantListWidget;
}

namespace CollabEdit {

class CollabPane : public QWidget {
    Q_OBJECT
public:
    CollabPane(CollabText::Identity::Identity identity,
               std::string replica_name,
               std::filesystem::path sidecar_dir,
               std::unique_ptr<CollabText::DocumentSession> session,
               QWidget *parent = nullptr);
    ~CollabPane() override;

    /// Current visible text rendered from the CRDT.
    std::string text() const;

    /// Stop sync timer, mark presence departed, flush remaining ops.
    QString shutdown();
    QString flush();
    bool isModified() const noexcept { return m_modified; }
    void markModified() { m_modified = true; emit changed(); }
    void markSaved() noexcept { m_modified = false; emit changed(); }

signals:
    void changed();
    void errorOccurred(const QString &message);

private slots:
    void onContentsChange(int position, int charsRemoved, int charsAdded);
    void onViewportScrolled();
    void syncCycle();
    void undoLocal();
    void redoLocal();
    void gremlinTick();
    void onChatMessageSent(const QString &body);
    void onAddComment(const QString &body);
    void onCommentClicked(const QString &commentId);
    void onChatAnchorClicked(int line);
    void setCommentResolved(const QString &id, bool resolved);
    void deleteComment(const QString &id);

private:
    void writePresence();
    void writeEphemeral();
    void applyRemoteCursorsAndFollow();
    void applyEditsPreservingScroll(const std::vector<CollabText::SessionTextEdit> &edits);
    void applyEditsToQt(const std::vector<CollabText::SessionTextEdit> &edits);
    void syncStreams();
    uint32_t qtPosToByteOffset(int qtPos) const;
    int      byteOffsetToQtPos(uint32_t byteOffset) const;

    CollabText::Identity::Identity m_identity;
    std::string m_replicaName;
    std::unique_ptr<CollabText::DocumentSession> m_session;
    CollabText::Identity::PresenceManager m_presence;
    CollabText::Identity::IdentityProjector m_projector;
    std::unique_ptr<CollabText::Crdt::StreamSync> m_streamSync;

    CollabText::Ui::CollabPlainTextEdit *m_edit = nullptr;
    CollabText::Ui::ParticipantListWidget *m_participantList = nullptr;
    CollabText::Ui::ChatPanelWidget *m_chatPanel = nullptr;
    CollabText::Ui::CommentsPanelWidget *m_commentsPanel = nullptr;
    QPushButton *m_followBtn = nullptr;
    QPushButton *m_gremlinBtn = nullptr;
    QTextDocument *m_qtDoc = nullptr;
    QTimer *m_syncTimer = nullptr;
    QTimer *m_gremlinTimer = nullptr;

    std::string m_sessionStarted;
    uint64_t m_ephemeralSeq = 0;
    uint64_t m_chatSeq = 0;
    uint64_t m_commentSeq = 0;
    size_t m_lastChatCount = 0;
    bool m_syncing = false;
    bool m_shutdownComplete = false;
    bool m_modified = false;
    bool m_recoveryErrorShown = false;

    // Follow mode
    bool m_following = false;
    bool m_followScrolling = false;
    QString m_followTargetName;

    // Coalescing for word-level undo
    enum class EditKind { None, Insert, Backspace, Other };
    EditKind m_lastEditKind = EditKind::None;
    qint64 m_lastEditTimeMs = 0;
    int m_lastEditEndQt = -1;
    bool m_lastInsertEndsOnWordBoundary = false;
    size_t m_lastUndoDepth = 0;

    // Viewport anchor cache (sent via ephemeral)
    std::optional<CollabText::Crdt::Anchor> m_viewportTopAnchor;
    std::optional<CollabText::Crdt::Anchor> m_viewportBottomAnchor;

    // Throttling state
    qint64 m_lastPresenceMs = 0;
    qint64 m_lastEphemeralMs = 0;
    int    m_lastEphemeralBytePos = -1;
    int    m_lastEphemeralByteAnchor = -1;
};

} // namespace CollabEdit
