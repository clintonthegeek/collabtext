#include "collabtext/CollabDocument.h"

#include <QPlainTextDocumentLayout>
#include <QTextCursor>

namespace CollabText {

CollabDocument::CollabDocument(QObject *parent)
    : QObject(parent)
    , m_crdt(new YrsDocument(this))
    , m_qtDoc(new QTextDocument(this))
{
    // QPlainTextEdit requires QPlainTextDocumentLayout
    m_qtDoc->setDocumentLayout(new QPlainTextDocumentLayout(m_qtDoc));

    // Disable QTextDocument's built-in undo — we use the CRDT's undo instead
    m_qtDoc->setUndoRedoEnabled(false);

    // When remote edits arrive via CRDT, push them into QTextDocument
    connect(m_crdt, &YrsDocument::remoteTextChanged,
            this, &CollabDocument::onRemoteTextChanged);

    // Forward update-produced signal for transport
    connect(m_crdt, &YrsDocument::updateProduced,
            this, &CollabDocument::updateReady);
}

CollabDocument::~CollabDocument() = default;

void CollabDocument::insertText(int position, const QString &text)
{
    // 1. Apply to CRDT (source of truth)
    m_crdt->insert(static_cast<uint32_t>(position), text);

    // 2. The CRDT observer will fire remoteTextChanged, which pushes to QTextDocument.
    //    But for local edits we want immediate feedback, so we also apply directly.
    m_suppressQtSignals = true;
    QTextCursor cursor(m_qtDoc);
    cursor.setPosition(position);
    cursor.insertText(text);
    m_suppressQtSignals = false;
}

void CollabDocument::removeText(int position, int length)
{
    m_crdt->remove(static_cast<uint32_t>(position), static_cast<uint32_t>(length));

    m_suppressQtSignals = true;
    QTextCursor cursor(m_qtDoc);
    cursor.setPosition(position);
    cursor.setPosition(position + length, QTextCursor::KeepAnchor);
    cursor.removeSelectedText();
    m_suppressQtSignals = false;
}

bool CollabDocument::undo()
{
    if (!m_crdt->canUndo())
        return false;

    bool ok = m_crdt->undo();
    if (ok)
        syncCrdtToQt();
    return ok;
}

bool CollabDocument::redo()
{
    if (!m_crdt->canRedo())
        return false;

    bool ok = m_crdt->redo();
    if (ok)
        syncCrdtToQt();
    return ok;
}

QByteArray CollabDocument::stickyIndexAt(int position, int8_t assoc)
{
    return m_crdt->createStickyIndex(static_cast<uint32_t>(position), assoc);
}

int CollabDocument::resolveSticky(const QByteArray &encoded) const
{
    return m_crdt->resolveStickyIndex(encoded);
}

void CollabDocument::onRemoteTextChanged(const QVector<TextDelta> &deltas)
{
    if (m_suppressQtSignals)
        return;

    // Apply CRDT deltas to QTextDocument as minimal edits
    m_suppressQtSignals = true;
    QTextCursor cursor(m_qtDoc);
    cursor.beginEditBlock();

    int pos = 0;
    for (const auto &delta : deltas) {
        switch (delta.tag) {
        case TextDelta::Retain:
            pos += static_cast<int>(delta.len);
            break;
        case TextDelta::Insert:
            cursor.setPosition(pos);
            cursor.insertText(delta.text);
            pos += delta.text.size();
            break;
        case TextDelta::Delete:
            cursor.setPosition(pos);
            cursor.setPosition(pos + static_cast<int>(delta.len),
                               QTextCursor::KeepAnchor);
            cursor.removeSelectedText();
            break;
        }
    }

    cursor.endEditBlock();
    m_suppressQtSignals = false;
}

void CollabDocument::syncCrdtToQt()
{
    // Full resync after undo/redo — replace entire QTextDocument content
    m_suppressQtSignals = true;
    QString content = m_crdt->text();
    QTextCursor cursor(m_qtDoc);
    cursor.select(QTextCursor::Document);
    cursor.insertText(content);
    m_suppressQtSignals = false;
}

} // namespace CollabText
