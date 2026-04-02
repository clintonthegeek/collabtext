#pragma once

#include "collabtext/CrdtEngine.h"

#include <QObject>
#include <QTextDocument>

namespace CollabText {

class CollabDocument : public QObject {
    Q_OBJECT

public:
    explicit CollabDocument(uint16_t replicaId = 0, QObject *parent = nullptr);
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
