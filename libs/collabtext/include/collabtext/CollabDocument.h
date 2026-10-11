#pragma once

#include "collabtext/CrdtEngine.h"
#include "collabtext/DocumentSession.h"

#include <QByteArray>
#include <QObject>
#include <QTextDocument>

#include <string>
#include <vector>

namespace CollabText {

class CollabDocument : public QObject {
    Q_OBJECT

public:
    /// Takes the allocator this document mints identities from. No default and no
    /// ActorId-only constructor: a document constructed with an implicit actor 0
    /// would emit operations indistinguishable from any other process's, which is
    /// the identity collision P02 exists to prevent
    /// (docs/specs/vnext-identity.md §3.7).
    explicit CollabDocument(Crdt::SharedIdentityAllocator allocator,
                            QObject *parent = nullptr);
    /// Binds the Qt document to a caller-owned durable session. The session
    /// must outlive this adapter; local edits are durably submitted through it.
    explicit CollabDocument(DocumentSession &session, QObject *parent = nullptr);
    ~CollabDocument() override;

    QTextDocument *qtDocument() const { return m_qtDoc; }
    CrdtEngine *engine() const { return m_engine; }
    DocumentSession *session() const { return m_session; }

    void insertText(int position, const QString &text);
    void removeText(int position, int length);
    [[nodiscard]] Result<ApplyOutcome> applyUpdate(const QByteArray &update);
    /// Apply session-produced deltas under scoped feedback suppression.
    void applySessionEdits(const std::vector<SessionTextEdit> &edits);

    bool undo();
    bool redo();

signals:
    void updateReady(const QByteArray &update);
    void operationError(const QString &message);
    void localEditApplied(int position, int charsRemoved, int charsAdded);

private:
    void initializeDocument(const std::string &text);
    void syncBackendToQt();
    void onQtContentsChanged();

    CrdtEngine *m_engine = nullptr;
    DocumentSession *m_session = nullptr;
    QTextDocument *m_qtDoc = nullptr;
    std::string m_backendText;
    bool m_suppressQtSignals = false;
};

} // namespace CollabText
