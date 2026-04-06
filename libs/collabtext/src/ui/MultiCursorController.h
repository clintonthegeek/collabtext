#pragma once

#include <QList>
#include <QObject>
#include <QTextCursor>
#include <QTextEdit>

class QTextDocument;

namespace CollabText::Ui {

struct RemoteCursor {
    int position = 0;
    int anchor = 0;
    QColor color;
    QString label;
};

class MultiCursorController : public QObject {
    Q_OBJECT
public:
    explicit MultiCursorController(QTextDocument *document, QObject *parent = nullptr);

    QTextDocument *document() const { return m_document; }

    int cursorCount() const;
    QTextCursor primaryCursor() const;
    void setPrimaryCursor(const QTextCursor &cursor);

    void addCursorAt(int position);
    void addCursorAbove();
    void addCursorBelow();
    void clearSecondaryCursors();

    void insertText(const QString &text);
    void deleteChar();
    void deletePreviousChar();
    void moveCursors(QTextCursor::MoveOperation op,
                     QTextCursor::MoveMode mode = QTextCursor::MoveAnchor);

    QList<QTextCursor> allCursors() const;
    QList<QTextEdit::ExtraSelection> secondarySelections() const;
    QList<int> secondaryCaretPositions() const;

    void setRemoteCursors(const QList<RemoteCursor> &cursors);
    QList<QTextEdit::ExtraSelection> remoteSelections() const;
    QList<RemoteCursor> remoteCursors() const { return m_remoteCursors; }

Q_SIGNALS:
    void cursorsChanged();

private:
    void mergeTouchingCursors();
    QList<QTextCursor> allCursorsSortedDescending() const;

    QTextDocument *m_document;
    QTextCursor m_primary;
    QList<QTextCursor> m_secondary;
    QList<RemoteCursor> m_remoteCursors;
};

} // namespace CollabText::Ui
