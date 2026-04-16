#include "ui/CollabPlainTextEdit.h"
#include "ui/CursorLabelWidget.h"

#include <QEvent>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QShortcutEvent>
#include <QTextBlock>
#include <QAbstractTextDocumentLayout>

#include <algorithm>

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

void CollabPlainTextEdit::setDocument(QTextDocument *document) {
    QPlainTextEdit::setDocument(document);
    // Recreate controller for the new document
    delete m_controller;
    m_controller = new MultiCursorController(document, this);
    connect(m_controller, &MultiCursorController::cursorsChanged,
            this, &CollabPlainTextEdit::syncExtraSelections);
}

void CollabPlainTextEdit::paintEvent(QPaintEvent *e) {
    QPlainTextEdit::paintEvent(e);

    QPainter painter(viewport());
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);

    for (int pos : m_controller->secondaryCaretPositions()) {
        drawSecondaryCaret(painter, pos, QColor(100, 149, 237));
    }

    // Draw remote cursors — resolve byte offsets to Qt positions
    QString docText = document()->toPlainText();
    QByteArray utf8 = docText.toUtf8();
    int maxPos = document()->characterCount() - 1;
    if (maxPos < 0) maxPos = 0;
    for (auto &rc : m_controller->remoteCursors()) {
        uint32_t clamped = qMin(rc.bytePosition, static_cast<uint32_t>(utf8.size()));
        int qtPos = qMin(QString::fromUtf8(utf8.data(), clamped).length(), maxPos);
        drawSecondaryCaret(painter, qtPos, rc.color);
    }
}

bool CollabPlainTextEdit::event(QEvent *e) {
    // QPlainTextEdit accepts ShortcutOverride for the standard editing
    // shortcuts (including Ctrl+Z / Ctrl+Shift+Z / Ctrl+Y), which prevents
    // the normal QAction/QShortcut routing from firing. We DON'T want that
    // for undo/redo, since the CRDT owns the undo stack — Qt's built-in
    // undo is disabled. Let those keys fall through to keyPressEvent below
    // so we can emit our own signals.
    if (e->type() == QEvent::ShortcutOverride) {
        auto *ke = static_cast<QKeyEvent *>(e);
        if (ke == QKeySequence::Undo || ke == QKeySequence::Redo) {
            ke->ignore();
            return true;
        }
    }
    return QPlainTextEdit::event(e);
}

void CollabPlainTextEdit::keyPressEvent(QKeyEvent *e) {
    // Undo/redo: route to the embedding app via signals so it can drive
    // the CRDT undo stack. Must be checked before delegating to the base
    // QPlainTextEdit, which would otherwise swallow these keys.
    if (e == QKeySequence::Undo) {
        emit undoRequested();
        e->accept();
        return;
    }
    if (e == QKeySequence::Redo) {
        emit redoRequested();
        e->accept();
        return;
    }

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

void CollabPlainTextEdit::scrollContentsBy(int dx, int dy) {
    QPlainTextEdit::scrollContentsBy(dx, dy);
    updateCursorLabels();
    emit viewportScrolled();
}

void CollabPlainTextEdit::setCommentHighlights(
        const QList<std::tuple<uint32_t, uint32_t, QColor>> &highlights) {
    m_commentSelections.clear();
    for (const auto &[startByte, endByte, color] : highlights) {
        int qtStart = byteOffsetToQtPos(startByte);
        int qtEnd = byteOffsetToQtPos(endByte);
        if (qtStart == qtEnd) continue;

        QTextEdit::ExtraSelection sel;
        QColor bg = color;
        bg.setAlpha(50);
        sel.format.setBackground(bg);
        QTextCursor cursor(document());
        cursor.setPosition(qtStart);
        cursor.setPosition(qtEnd, QTextCursor::KeepAnchor);
        sel.cursor = cursor;
        m_commentSelections.append(sel);
    }
    syncExtraSelections();
}

void CollabPlainTextEdit::syncExtraSelections() {
    QList<QTextEdit::ExtraSelection> selections;
    selections.append(m_controller->secondarySelections());
    selections.append(m_controller->remoteSelections());
    selections.append(m_commentSelections);
    setExtraSelections(selections);
    viewport()->update();
    updateCursorLabels();
}

void CollabPlainTextEdit::updateCursorLabels() {
    QString docText = document()->toPlainText();
    QByteArray utf8 = docText.toUtf8();
    int maxPos = document()->characterCount() - 1;
    if (maxPos < 0) maxPos = 0;

    QSet<QString> activeIds;

    for (const auto &rc : m_controller->remoteCursors()) {
        if (rc.identityId.isEmpty()) continue;
        activeIds.insert(rc.identityId);

        // Resolve byte offset to Qt position
        uint32_t clamped = qMin(rc.bytePosition, static_cast<uint32_t>(utf8.size()));
        int qtPos = qMin(QString::fromUtf8(utf8.data(), clamped).length(), maxPos);

        // Get screen coordinates for this position
        QTextCursor tc(document());
        tc.setPosition(qtPos);
        QRect caretRect = cursorRect(tc);

        // Check if cursor is in the visible viewport
        QRect vpRect = viewport()->rect();
        if (!vpRect.intersects(caretRect)) {
            if (auto *lbl = m_cursorLabels.value(rc.identityId))
                lbl->hide();
            continue;
        }

        // Get or create label widget
        CursorLabelWidget *lbl = m_cursorLabels.value(rc.identityId);
        if (!lbl) {
            lbl = new CursorLabelWidget(viewport());
            m_cursorLabels.insert(rc.identityId, lbl);
        }

        lbl->setLabel(rc.label, rc.color);

        // Position: above the caret, or below if at top edge
        bool flipBelow = (caretRect.top() < lbl->sizeHint().height() + 4);
        QPoint pos(caretRect.left(), flipBelow ? caretRect.bottom() : caretRect.top());
        lbl->showAtPosition(pos, rc.cursorVersion, flipBelow);
    }

    // Remove labels for departed participants
    for (auto it = m_cursorLabels.begin(); it != m_cursorLabels.end(); ) {
        if (!activeIds.contains(it.key())) {
            delete it.value();
            it = m_cursorLabels.erase(it);
        } else {
            ++it;
        }
    }

    // Resolve label collisions: nudge overlapping labels downward so
    // they stack instead of rendering on top of each other.
    QList<CursorLabelWidget*> visibleLabels;
    for (auto it = m_cursorLabels.cbegin(); it != m_cursorLabels.cend(); ++it) {
        if (it.value()->isVisible())
            visibleLabels.append(it.value());
    }
    if (visibleLabels.size() > 1) {
        std::sort(visibleLabels.begin(), visibleLabels.end(),
            [](const CursorLabelWidget *a, const CursorLabelWidget *b) {
                return a->y() < b->y()
                    || (a->y() == b->y() && a->x() < b->x());
            });
        for (int i = 0; i < visibleLabels.size(); ++i) {
            for (int j = i + 1; j < visibleLabels.size(); ++j) {
                if (visibleLabels[i]->geometry().intersects(
                        visibleLabels[j]->geometry())) {
                    visibleLabels[j]->move(
                        visibleLabels[j]->x(),
                        visibleLabels[i]->geometry().bottom() + 2);
                }
            }
        }
    }
}

int CollabPlainTextEdit::byteOffsetToQtPos(uint32_t byteOff) const {
    QByteArray utf8 = document()->toPlainText().toUtf8();
    uint32_t clamped = qMin(byteOff, static_cast<uint32_t>(utf8.size()));
    return QString::fromUtf8(utf8.data(), static_cast<int>(clamped)).length();
}

uint32_t CollabPlainTextEdit::qtPosToByteOffset(int qtPos) const {
    QString docText = document()->toPlainText();
    if (qtPos <= 0) return 0;
    if (qtPos >= docText.length()) return static_cast<uint32_t>(docText.toUtf8().size());
    return static_cast<uint32_t>(docText.left(qtPos).toUtf8().size());
}

uint32_t CollabPlainTextEdit::topVisibleByteOffset() const {
    if (document()->isEmpty()) return 0;
    // cursorForPosition respects visual line wrapping, not logical blocks,
    // so (0, 0) gives us the first visible WRAPPED line, which is what we
    // want for stable scroll restoration after remote edits.
    QTextCursor c = cursorForPosition(QPoint(0, 0));
    return qtPosToByteOffset(c.position());
}

uint32_t CollabPlainTextEdit::bottomVisibleByteOffset() const {
    if (document()->isEmpty()) return 0;
    // Degenerate case: a zero-sized viewport has no visible content, so
    // bottom == top == 0. Without this guard we'd pass QPoint(-1, -1)
    // to cursorForPosition, which clamps to position 0 anyway but
    // reading the code would obscure the intent.
    if (viewport()->width() <= 0 || viewport()->height() <= 0) return 0;
    QPoint bottomRight(viewport()->width() - 1, viewport()->height() - 1);
    QTextCursor c = cursorForPosition(bottomRight);
    // If the bottom-right falls past the end of the document,
    // cursorForPosition clamps to the last valid position, but we want
    // the total document byte length in that case.
    int lastPos = document()->characterCount() - 1;
    if (c.position() >= lastPos) {
        return static_cast<uint32_t>(document()->toPlainText().toUtf8().size());
    }
    return qtPosToByteOffset(c.position());
}

void CollabPlainTextEdit::scrollByteOffsetToTop(uint32_t byteOff,
                                                 bool keepCursorVisible) {
    int qtPos = byteOffsetToQtPos(byteOff);
    QTextCursor target(document());
    target.setPosition(qtPos);

    auto *bar = verticalScrollBar();
    if (!bar) return;

    QTextBlock targetBlock = target.block();
    if (!targetBlock.isValid()) return;

    // Compute the exact visual line number. firstLineNumber() gives the
    // absolute visual line of the block's start (accounts for word-wrap
    // in all preceding blocks). We add the visual line index within the
    // block for positions past the block start.
    int visualLine = targetBlock.firstLineNumber();

    int posInBlock = qtPos - targetBlock.position();
    if (posInBlock > 0) {
        QTextLayout *layout = targetBlock.layout();
        if (layout && layout->lineCount() > 0) {
            QTextLine line = layout->lineForTextPosition(posInBlock);
            if (line.isValid()) {
                visualLine += line.lineNumber();
            }
        }
    }

    bar->setValue(visualLine);

    if (keepCursorVisible) {
        ensureCursorVisible();
    }
}

} // namespace CollabText::Ui
