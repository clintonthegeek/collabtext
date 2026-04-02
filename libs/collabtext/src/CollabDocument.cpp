#include "collabtext/CollabDocument.h"

#include <QPlainTextDocumentLayout>
#include <QTextCursor>

namespace CollabText {

CollabDocument::CollabDocument(uint16_t replicaId, QObject *parent)
    : QObject(parent)
    , m_engine(new CrdtEngine(replicaId))
    , m_qtDoc(new QTextDocument(this))
{
    m_qtDoc->setDocumentLayout(new QPlainTextDocumentLayout(m_qtDoc));
    m_qtDoc->setUndoRedoEnabled(false);
}

CollabDocument::~CollabDocument()
{
    delete m_engine;
}

void CollabDocument::insertText(int position, const QString &text)
{
    m_engine->insert(position, text.toStdString());

    m_suppressQtSignals = true;
    QTextCursor cursor(m_qtDoc);
    cursor.setPosition(position);
    cursor.insertText(text);
    m_suppressQtSignals = false;
}

void CollabDocument::removeText(int position, int length)
{
    m_engine->remove(position, length);

    m_suppressQtSignals = true;
    QTextCursor cursor(m_qtDoc);
    cursor.setPosition(position);
    cursor.setPosition(position + length, QTextCursor::KeepAnchor);
    cursor.removeSelectedText();
    m_suppressQtSignals = false;
}

bool CollabDocument::undo()
{
    bool ok = m_engine->undo();
    if (ok) syncEngineToQt();
    return ok;
}

bool CollabDocument::redo()
{
    bool ok = m_engine->redo();
    if (ok) syncEngineToQt();
    return ok;
}

void CollabDocument::syncEngineToQt()
{
    m_suppressQtSignals = true;
    QString content = QString::fromStdString(m_engine->text());
    QTextCursor cursor(m_qtDoc);
    cursor.select(QTextCursor::Document);
    cursor.insertText(content);
    m_suppressQtSignals = false;
}

} // namespace CollabText
