#include "ui/CollabPlainTextEdit.h"

#include <QPainter>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QTextBlock>
#include <QScrollBar>
#include <QAbstractTextDocumentLayout>

namespace CollabText::Ui {

CollabPlainTextEdit::CollabPlainTextEdit(QWidget *parent)
    : QPlainTextEdit(parent)
    , m_controller(new MultiCursorController(document(), this))
{
    connect(m_controller, &MultiCursorController::cursorsChanged,
            this, &CollabPlainTextEdit::syncExtraSelections);

    connect(this, &QPlainTextEdit::cursorPositionChanged, this, [this]() {
        if (!m_handlingKey) {
            m_controller->setPrimaryCursor(textCursor());
        }
    });
}

void CollabPlainTextEdit::paintEvent(QPaintEvent *e) {
    QPlainTextEdit::paintEvent(e);

    QPainter painter(viewport());
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);

    for (int pos : m_controller->secondaryCaretPositions()) {
        drawSecondaryCaret(painter, pos, QColor(100, 149, 237));
    }

    for (auto &rc : m_controller->remoteCursors()) {
        drawSecondaryCaret(painter, rc.position, rc.color);
    }
}

void CollabPlainTextEdit::keyPressEvent(QKeyEvent *e) {
    if (e->key() == Qt::Key_Escape && m_controller->cursorCount() > 1) {
        m_controller->clearSecondaryCursors();
        e->accept();
        return;
    }

    if (e->modifiers() == (Qt::ControlModifier | Qt::AltModifier)) {
        if (e->key() == Qt::Key_Up) {
            m_controller->addCursorAbove();
            e->accept();
            return;
        }
        if (e->key() == Qt::Key_Down) {
            m_controller->addCursorBelow();
            e->accept();
            return;
        }
    }

    if (m_controller->cursorCount() == 1) {
        QPlainTextEdit::keyPressEvent(e);
        return;
    }

    m_handlingKey = true;

    if (e->key() == Qt::Key_Backspace) {
        m_controller->deletePreviousChar();
    } else if (e->key() == Qt::Key_Delete) {
        m_controller->deleteChar();
    } else if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
        m_controller->insertText("\n");
    } else if (e->key() == Qt::Key_Left) {
        m_controller->moveCursors(QTextCursor::Left,
            e->modifiers() & Qt::ShiftModifier ? QTextCursor::KeepAnchor : QTextCursor::MoveAnchor);
    } else if (e->key() == Qt::Key_Right) {
        m_controller->moveCursors(QTextCursor::Right,
            e->modifiers() & Qt::ShiftModifier ? QTextCursor::KeepAnchor : QTextCursor::MoveAnchor);
    } else if (e->key() == Qt::Key_Up) {
        m_controller->moveCursors(QTextCursor::Up,
            e->modifiers() & Qt::ShiftModifier ? QTextCursor::KeepAnchor : QTextCursor::MoveAnchor);
    } else if (e->key() == Qt::Key_Down) {
        m_controller->moveCursors(QTextCursor::Down,
            e->modifiers() & Qt::ShiftModifier ? QTextCursor::KeepAnchor : QTextCursor::MoveAnchor);
    } else if (e->key() == Qt::Key_Home) {
        m_controller->moveCursors(QTextCursor::StartOfLine,
            e->modifiers() & Qt::ShiftModifier ? QTextCursor::KeepAnchor : QTextCursor::MoveAnchor);
    } else if (e->key() == Qt::Key_End) {
        m_controller->moveCursors(QTextCursor::EndOfLine,
            e->modifiers() & Qt::ShiftModifier ? QTextCursor::KeepAnchor : QTextCursor::MoveAnchor);
    } else if (!e->text().isEmpty() && e->text().at(0).isPrint()) {
        m_controller->insertText(e->text());
    } else {
        QPlainTextEdit::keyPressEvent(e);
        m_handlingKey = false;
        return;
    }

    setTextCursor(m_controller->primaryCursor());
    m_handlingKey = false;
    e->accept();
}

void CollabPlainTextEdit::mousePressEvent(QMouseEvent *e) {
    if (e->modifiers() & Qt::AltModifier) {
        QTextCursor clickCursor = cursorForPosition(e->pos());
        m_controller->addCursorAt(clickCursor.position());
        e->accept();
        return;
    }

    if (m_controller->cursorCount() > 1) {
        m_controller->clearSecondaryCursors();
    }
    QPlainTextEdit::mousePressEvent(e);
}

void CollabPlainTextEdit::drawSecondaryCaret(QPainter &painter, int position,
                                              const QColor &color) {
    QTextCursor c(document());
    c.setPosition(position);
    QTextBlock block = c.block();
    if (!block.isValid()) return;

    QTextLayout *layout = block.layout();
    if (!layout) return;

    int relativePos = position - block.position();
    QTextLine line = layout->lineForTextPosition(relativePos);
    if (!line.isValid()) return;

    qreal x = line.cursorToX(relativePos);
    QRectF blockRect = blockBoundingGeometry(block).translated(contentOffset());
    qreal y = blockRect.top() + line.y();
    qreal height = line.height();

    painter.fillRect(QRectF(x, y, 2.0, height), color);
}

void CollabPlainTextEdit::syncExtraSelections() {
    QList<QTextEdit::ExtraSelection> selections;
    selections.append(m_controller->secondarySelections());
    selections.append(m_controller->remoteSelections());
    setExtraSelections(selections);
    viewport()->update();
}

} // namespace CollabText::Ui
