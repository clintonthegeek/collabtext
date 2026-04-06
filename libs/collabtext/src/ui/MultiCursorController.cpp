#include "ui/MultiCursorController.h"
#include <QTextBlock>
#include <QTextDocument>
#include <algorithm>

namespace CollabText::Ui {

MultiCursorController::MultiCursorController(QTextDocument *document, QObject *parent)
    : QObject(parent)
    , m_document(document)
    , m_primary(document)
{
}

int MultiCursorController::cursorCount() const {
    return 1 + m_secondary.size();
}

QTextCursor MultiCursorController::primaryCursor() const {
    return m_primary;
}

void MultiCursorController::setPrimaryCursor(const QTextCursor &cursor) {
    m_primary = cursor;
    emit cursorsChanged();
}

void MultiCursorController::addCursorAt(int position) {
    QTextCursor c(m_document);
    c.setPosition(position);
    m_secondary.append(c);
    mergeTouchingCursors();
    emit cursorsChanged();
}

void MultiCursorController::addCursorAbove() {
    auto all = allCursors();
    for (auto &c : all) {
        int col = c.position() - c.block().position();
        QTextBlock prevBlock = c.block().previous();
        if (!prevBlock.isValid()) continue;
        int newPos = prevBlock.position() + qMin(col, prevBlock.length() - 1);
        QTextCursor above(m_document);
        above.setPosition(newPos);
        if (above.blockNumber() != c.blockNumber()) {
            m_secondary.append(above);
        }
    }
    mergeTouchingCursors();
    emit cursorsChanged();
}

void MultiCursorController::addCursorBelow() {
    auto all = allCursors();
    for (auto &c : all) {
        int col = c.position() - c.block().position();
        QTextBlock nextBlock = c.block().next();
        if (!nextBlock.isValid()) continue;
        int newPos = nextBlock.position() + qMin(col, nextBlock.length() - 1);
        QTextCursor below(m_document);
        below.setPosition(newPos);
        if (below.blockNumber() != c.blockNumber()) {
            m_secondary.append(below);
        }
    }
    mergeTouchingCursors();
    emit cursorsChanged();
}

void MultiCursorController::clearSecondaryCursors() {
    m_secondary.clear();
    emit cursorsChanged();
}

void MultiCursorController::insertText(const QString &text) {
    auto cursors = allCursorsSortedDescending();
    if (cursors.isEmpty()) return;
    cursors.first().beginEditBlock();
    for (int i = 0; i < cursors.size(); ++i) {
        if (i > 0) cursors[i].joinPreviousEditBlock();
        cursors[i].insertText(text);
        cursors[i].endEditBlock();
    }
    emit cursorsChanged();
}

void MultiCursorController::deleteChar() {
    auto cursors = allCursorsSortedDescending();
    if (cursors.isEmpty()) return;
    cursors.first().beginEditBlock();
    for (int i = 0; i < cursors.size(); ++i) {
        if (i > 0) cursors[i].joinPreviousEditBlock();
        cursors[i].deleteChar();
        cursors[i].endEditBlock();
    }
    emit cursorsChanged();
}

void MultiCursorController::deletePreviousChar() {
    auto cursors = allCursorsSortedDescending();
    if (cursors.isEmpty()) return;
    cursors.first().beginEditBlock();
    for (int i = 0; i < cursors.size(); ++i) {
        if (i > 0) cursors[i].joinPreviousEditBlock();
        cursors[i].deletePreviousChar();
        cursors[i].endEditBlock();
    }
    emit cursorsChanged();
}

void MultiCursorController::moveCursors(QTextCursor::MoveOperation op,
                                         QTextCursor::MoveMode mode) {
    m_primary.movePosition(op, mode);
    for (auto &c : m_secondary) {
        c.movePosition(op, mode);
    }
    mergeTouchingCursors();
    emit cursorsChanged();
}

QList<QTextCursor> MultiCursorController::allCursors() const {
    QList<QTextCursor> result;
    result.reserve(1 + m_secondary.size());
    result.append(m_primary);
    result.append(m_secondary);
    return result;
}

QList<QTextEdit::ExtraSelection> MultiCursorController::secondarySelections() const {
    QList<QTextEdit::ExtraSelection> result;
    for (auto &c : m_secondary) {
        if (c.hasSelection()) {
            QTextEdit::ExtraSelection sel;
            sel.cursor = c;
            sel.format.setBackground(QColor(100, 149, 237, 80));
            result.append(sel);
        }
    }
    return result;
}

QList<int> MultiCursorController::secondaryCaretPositions() const {
    QList<int> result;
    result.reserve(m_secondary.size());
    for (auto &c : m_secondary) {
        result.append(c.position());
    }
    return result;
}

void MultiCursorController::setRemoteCursors(const QList<RemoteCursor> &cursors) {
    m_remoteCursors = cursors;
    emit cursorsChanged();
}

/// Convert a byte offset in UTF-8 engine space to a Qt character position.
static int byteOffsetToQtPos(const QString &docText, uint32_t byteOffset) {
    QByteArray utf8 = docText.toUtf8();
    uint32_t clamped = qMin(byteOffset, static_cast<uint32_t>(utf8.size()));
    return QString::fromUtf8(utf8.data(), clamped).length();
}

QList<QTextEdit::ExtraSelection> MultiCursorController::remoteSelections() const {
    QList<QTextEdit::ExtraSelection> result;
    if (m_remoteCursors.isEmpty()) return result;

    QString docText = m_document->toPlainText();
    int maxPos = m_document->characterCount() - 1;
    if (maxPos < 0) maxPos = 0;

    for (auto &rc : m_remoteCursors) {
        int qtPos = qMin(byteOffsetToQtPos(docText, rc.bytePosition), maxPos);
        int qtAnchor = qMin(byteOffsetToQtPos(docText, rc.byteAnchor), maxPos);

        QTextEdit::ExtraSelection sel;
        sel.cursor = QTextCursor(m_document);
        if (qtPos != qtAnchor) {
            sel.cursor.setPosition(qtAnchor);
            sel.cursor.setPosition(qtPos, QTextCursor::KeepAnchor);
            sel.format.setBackground(QColor(rc.color.red(), rc.color.green(),
                                            rc.color.blue(), 50));
        } else {
            sel.cursor.setPosition(qtPos);
        }
        result.append(sel);
    }
    return result;
}

void MultiCursorController::mergeTouchingCursors() {
    QList<QTextCursor> merged;
    int primaryPos = m_primary.position();
    for (auto &c : m_secondary) {
        if (c.position() == primaryPos) continue;
        bool duplicate = false;
        for (auto &m : merged) {
            if (c.position() == m.position()) { duplicate = true; break; }
        }
        if (!duplicate) merged.append(c);
    }
    m_secondary = merged;
}

QList<QTextCursor> MultiCursorController::allCursorsSortedDescending() const {
    auto all = allCursors();
    std::sort(all.begin(), all.end(), [](const QTextCursor &a, const QTextCursor &b) {
        return a.position() > b.position();
    });
    return all;
}

} // namespace CollabText::Ui
