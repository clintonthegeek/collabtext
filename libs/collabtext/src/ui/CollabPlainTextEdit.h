#pragma once

#include "ui/MultiCursorController.h"
#include <QPlainTextEdit>

namespace CollabText::Ui {

class CollabPlainTextEdit : public QPlainTextEdit {
    Q_OBJECT
public:
    explicit CollabPlainTextEdit(QWidget *parent = nullptr);

    MultiCursorController *multiCursorController() const { return m_controller; }

protected:
    void paintEvent(QPaintEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;

private:
    void drawSecondaryCaret(QPainter &painter, int position, const QColor &color);
    void syncExtraSelections();

    MultiCursorController *m_controller;
    bool m_handlingKey = false;
};

} // namespace CollabText::Ui
