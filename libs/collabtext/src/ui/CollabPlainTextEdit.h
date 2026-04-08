#pragma once

#include "ui/MultiCursorController.h"
#include <QHash>
#include <QPlainTextEdit>

namespace CollabText::Ui {

class CursorLabelWidget;

class CollabPlainTextEdit : public QPlainTextEdit {
    Q_OBJECT
public:
    explicit CollabPlainTextEdit(QWidget *parent = nullptr);

    void setDocument(QTextDocument *document);
    MultiCursorController *multiCursorController() const { return m_controller; }

signals:
    /// Emitted when the user presses the platform's standard undo shortcut.
    /// QPlainTextEdit normally consumes this internally; we re-route it so
    /// that the embedding app can drive the CRDT undo stack.
    void undoRequested();

    /// Emitted when the user presses the platform's standard redo shortcut.
    void redoRequested();

public:
    /// Byte offset (UTF-8) of the character at the top-left of the viewport.
    /// Returns 0 for an empty document. Wrap-aware: returns the byte offset
    /// of the visual line at the top, not the containing paragraph.
    uint32_t topVisibleByteOffset() const;

    /// Byte offset (UTF-8) of the character just past the bottom-right
    /// of the viewport. Returns 0 for an empty document and returns the
    /// document's full UTF-8 byte length when the viewport shows the
    /// end of the document.
    uint32_t bottomVisibleByteOffset() const;

protected:
    bool event(QEvent *e) override;
    void paintEvent(QPaintEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void scrollContentsBy(int dx, int dy) override;

private:
    void drawSecondaryCaret(QPainter &painter, int position, const QColor &color);
    void syncExtraSelections();
    void updateCursorLabels();

    /// Convert a UTF-8 byte offset into a UTF-16 QTextDocument position.
    /// Precondition: byteOff must fall on a UTF-8 character boundary.
    /// All callers in the scroll-stability pipeline satisfy this (offsets
    /// come from either cursor positions or Buffer::resolve_anchor, both
    /// of which are boundary-aligned by construction).
    int byteOffsetToQtPos(uint32_t byteOff) const;

    /// Convert a UTF-16 QTextDocument position into a UTF-8 byte offset.
    uint32_t qtPosToByteOffset(int qtPos) const;

    MultiCursorController *m_controller;
    bool m_handlingKey = false;
    QHash<QString, CursorLabelWidget*> m_cursorLabels;
};

} // namespace CollabText::Ui
