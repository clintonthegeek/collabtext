#pragma once

#include <QPropertyAnimation>
#include <QTimer>
#include <QWidget>

namespace CollabText::Ui {

class CursorLabelWidget : public QWidget {
    Q_OBJECT
    Q_PROPERTY(qreal opacity READ opacity WRITE setOpacity)
public:
    explicit CursorLabelWidget(QWidget *viewport);

    void setLabel(const QString &name, const QColor &color);
    void showAtPosition(const QPoint &pos, quint64 cursorVersion, bool flipBelow = false);
    void scheduleFade();
    void cancelFade();

    QSize sizeHint() const override;

    qreal opacity() const { return m_opacity; }
    void setOpacity(qreal opacity);

protected:
    void paintEvent(QPaintEvent *) override;

private:
    void onFadeTimerTimeout();
    void onFadeFinished();

    QString m_name;
    QColor m_color;
    qreal m_opacity = 1.0;
    quint64 m_lastCursorVersion = 0;
    QTimer m_fadeTimer;
    QPropertyAnimation *m_fadeAnim = nullptr;
};

} // namespace CollabText::Ui
