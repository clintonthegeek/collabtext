#pragma once

#include "collabtext/CrdtEngine.h"

#include <QObject>
#include <QTextDocument>

namespace CollabText {

class CollabDocument : public QObject {
    Q_OBJECT

public:
    /// Takes the allocator this document mints identities from. No default and no
    /// ActorId-only constructor: a document constructed with an implicit actor 0
    /// would emit operations indistinguishable from any other process's, which is
    /// the identity collision P02 exists to prevent
    /// (docs/specs/vnext-identity.md §3.7).
    explicit CollabDocument(Crdt::SharedAllocator allocator,
                            QObject *parent = nullptr);
    ~CollabDocument() override;

    QTextDocument *qtDocument() const { return m_qtDoc; }
    CrdtEngine *engine() const { return m_engine; }

    void insertText(int position, const QString &text);
    void removeText(int position, int length);

    bool undo();
    bool redo();

signals:
    void updateReady(const QByteArray &update);

private:
    void syncEngineToQt();

    CrdtEngine *m_engine = nullptr;
    QTextDocument *m_qtDoc = nullptr;
    bool m_suppressQtSignals = false;
};

} // namespace CollabText
