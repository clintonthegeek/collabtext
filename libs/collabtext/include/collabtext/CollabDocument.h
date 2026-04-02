#pragma once

#include "collabtext/YrsWrapper.h"

#include <QObject>
#include <QTextCursor>
#include <QTextDocument>

namespace CollabText {

// Bridges a YrsDocument (CRDT) with a QTextDocument (rendering surface).
//
// Local edits on the QTextDocument are intercepted and routed through the
// CRDT. Remote updates from peers are applied to the CRDT and then pushed
// to the QTextDocument as minimal diffs.
//
// Usage:
//   auto *doc = new CollabDocument(this);
//   myTextEdit->setDocument(doc->qtDocument());
//   // edits via QTextCursor on qtDocument() are captured automatically
//   // remote updates arrive via doc->crdt()->applyUpdate(data)
//
class CollabDocument : public QObject {
    Q_OBJECT

public:
    explicit CollabDocument(QObject *parent = nullptr);
    ~CollabDocument() override;

    QTextDocument *qtDocument() const { return m_qtDoc; }
    YrsDocument *crdt() const { return m_crdt; }

    // Call from your widget's keyPressEvent INSTEAD of letting it hit
    // QTextDocument directly. This routes the edit through the CRDT.
    void insertText(int position, const QString &text);
    void removeText(int position, int length);

    // Undo/redo routed through the CRDT (not QTextDocument's stack).
    bool undo();
    bool redo();

    // Cursor tracking via CRDT-anchored positions
    QByteArray stickyIndexAt(int position, int8_t assoc = 0);
    int resolveSticky(const QByteArray &encoded) const;

signals:
    // Forwarded from YrsDocument — an encoded update ready for transport.
    void updateReady(const QByteArray &update);

private slots:
    void onRemoteTextChanged(const QVector<TextDelta> &deltas);

private:
    void syncCrdtToQt();

    YrsDocument *m_crdt = nullptr;
    QTextDocument *m_qtDoc = nullptr;

    // Suppress QTextDocument signals while we're pushing CRDT state into it
    bool m_suppressQtSignals = false;
};

} // namespace CollabText
