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

    MultiCursorController *m_controller;
    bool m_handlingKey = false;
    QHash<QString, CursorLabelWidget*> m_cursorLabels;
};

} // namespace CollabText::Ui
