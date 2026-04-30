#pragma once

#include "collabtext/Identity.h"
#include "collabtext/IdentityProjector.h"
#include "collabtext/PresenceManager.h"
#include "crdt/Buffer.h"
#include "crdt/FileSync.h"

#include <QWidget>

#include <filesystem>
#include <string>

class QTextDocument;
class QTimer;

namespace CollabText::Ui {
class CollabPlainTextEdit;
class ParticipantListWidget;
}

namespace CollabEdit {

class CollabPane : public QWidget {
    Q_OBJECT
public:
    CollabPane(CollabText::Identity::Identity identity,
               std::string replica_name,
               std::filesystem::path sidecar_dir,
               const std::string &seed_text,
               QWidget *parent = nullptr);
    ~CollabPane() override;

    /// Current visible text rendered from the CRDT.
    std::string text() const;

    /// Stop sync timer, mark presence departed, flush remaining ops.
    void shutdown();

private slots:
    void onContentsChange(int position, int charsRemoved, int charsAdded);
    void syncCycle();

private:
    void writePresence();
    void writeEphemeral();
    void applyRemoteCursors();
    uint32_t qtPosToByteOffset(int qtPos) const;
    int      byteOffsetToQtPos(uint32_t byteOffset) const;

    CollabText::Identity::Identity m_identity;
    std::string m_replicaName;

    CollabText::Crdt::Buffer m_buffer;
    CollabText::Crdt::FileSync m_sync;
    CollabText::Identity::PresenceManager m_presence;
    CollabText::Identity::IdentityProjector m_projector;

    CollabText::Ui::CollabPlainTextEdit *m_edit = nullptr;
    CollabText::Ui::ParticipantListWidget *m_participantList = nullptr;
    QTextDocument *m_qtDoc = nullptr;
    QTimer *m_syncTimer = nullptr;

    std::string m_sessionStarted;
    uint64_t m_ephemeralSeq = 0;
    bool m_syncing = false;
    bool m_shutdown = false;
};

} // namespace CollabEdit
