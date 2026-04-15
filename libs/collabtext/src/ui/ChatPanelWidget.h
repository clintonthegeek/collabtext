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

    /// Append a message. If anchorLine >= 1, show a clickable "(line N)" link.
    void addMessage(const QString &authorName, const QString &body,
                    const QString &timestamp, const QColor &authorColor,
                    int anchorLine = -1);

signals:
    void messageSent(const QString &body);
    /// Emitted when the user clicks a "(line N)" link in a chat message.
    void anchorClicked(int line);

private:
    QListWidget *m_list;
    QLineEdit *m_input;
};

} // namespace CollabText::Ui
