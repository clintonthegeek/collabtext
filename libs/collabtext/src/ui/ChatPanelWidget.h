#pragma once
#include <QColor>
#include <QLineEdit>
#include <QListWidget>
#include <QWidget>

namespace CollabText::Ui {

class ChatPanelWidget : public QWidget {
    Q_OBJECT
public:
    explicit ChatPanelWidget(QWidget *parent = nullptr);

    void addMessage(const QString &authorName, const QString &body,
                    const QString &timestamp, const QColor &authorColor);

signals:
    void messageSent(const QString &body);

private:
    QListWidget *m_list;
    QLineEdit *m_input;
};

} // namespace CollabText::Ui
